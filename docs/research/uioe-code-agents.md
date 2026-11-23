# UIOE research: code agents and their tool interfaces

Status: research note, 2026-11-23. No code was written and nothing was measured by katana for this note.
Scope: how existing coding agents give a model access to code, route and limit search, keep investigation state,
verify, and build the context the model sees, and what the katana UIOE (Unified Intelligence Orchestration
Engine) should take, adapt or reject.

Evidence labels used everywhere in this document:

- **VERIFIED (V)**: I read it at the cited source in this session (URL in section 19, accessed 2026-11-23).
  Where a figure appears, it is the figure the source states; I did not reproduce any experiment.
- **REMEMBERED (R)**: from my own prior knowledge, not re-checked against a source. Treat as a lead, not a fact.

Where a numeric result is quoted, the benchmark, the model and the date of the system matter; most of these
numbers are from 2024 to early 2025 with models far stronger than a local 3B-active MoE. They show what
matters, not what katana will get.

## 0. The target architecture and what this note is for

The owner's UIOE diagram (2026-11-23):

```
USER INPUT -> UIOE (Interpret . Classify . Route . Retrieve . Orchestrate)
           -> SOURCES (Filesystem, Web Search, Financial APIs, Code Repositories, Legal Sources, Databases)
           -> CONTEXT BUILDER (selects and organises the relevant data)
           -> LLM EXECUTION (receives the request and the prepared context)
```

Two properties of this diagram drive the whole note:

1. The UIOE runs **before** the model. The model receives a request plus a prepared context. This is the
   opposite of the dominant agent design, where the model is the router and calls tools one step at a time.
2. It is **general**: code repositories are one source among several. A code-agent study therefore has to be
   read twice: once for "how do they search code", and once for "how do they treat every other source and how
   do they assemble what the model sees".

The constraint set (from `CLAUDE.md` and the brief): local small model (Qwen3.6-35B-A3B MoE), one to two
minutes per step, no embeddings and no external service in the core, deterministic, explainable. Every design
conclusion below is filtered through those constraints. The cost model is the key one: with 1 to 2 minutes per
step, **each avoided model step is worth more than any retrieval quality gain that costs a step**. That is why
most "the model chooses and iterates" designs (SWE-agent, OpenHands, Claude Code, Cline) are expensive for
katana, and why the "engine decides first, model confirms" designs (Agentless, Aider repo map, Cody, Cursor
codebase search, katana's own evidence packet) are the relevant lineage.

What katana already has (from `docs/code-intelligence.md` sections 5 to 8, `docs/tools.md`,
`docs/code-index-lifecycle.md`; I read the headings and the cited excerpts, not every line):

- A question interpreter that turns the request into a typed structure (type overview/explain/locate/change/debug,
  terms with kind and document frequency, relation bits such as who-calls, where-defined, works/fails pair).
- An investigation orchestrator as a state machine (interpret, ensure index, exact entities, optional expansion
  by one model call, search by CSSE with a ranker, structure per hypothesis, diagnose, optional language-server
  confirmation, select under a token budget, emit).
- An evidence packet appended to the user turn with per-line grading (exact, derived, probable), a mandatory
  "Not found" section and instructions to the model on how far to trust each grade.
- An index with levels N0 to N4 built by a crawler, queried by tools (Find, Symbols, Grep, Glob, Read, LSP).

So katana is already, for code, a pre-LLM context builder. The question this note answers is what the field
suggests for (a) making it general across sources, (b) the parts of the agent loop that remain, and (c) what is
still missing.

## 1. Reading guide: the six questions asked of every system

For each system the sections below answer:

- (a) How information reaches the model: observation format, size, truncation.
- (b) Who decides which tool or strategy: the model, a rule, a router.
- (c) State and memory of the investigation.
- (d) Verification: tests, linter, reproduction.
- (e) What public benchmarks showed (numbers only if checked at the source).
- (f) Take, adapt or reject for the UIOE.
- Plus, per the 2026-11-23 refocus: (g) is there a **pre-LLM stage** that interprets, classifies and routes,
  (h) how the **context builder** selects, orders, budgets, summarises and cites, (i) how **non-code sources**
  (docs, web, issues, databases, MCP) are handled.

## 2. SWE-agent and the agent-computer interface (ACI)

Sources: paper arXiv:2405.15793 and its HTML version, the SWE-agent documentation page on ACI, the
mini-swe-agent README.

### 2.1 What it is

SWE-agent (Yang, Jimenez, Wettig, Lieret, Yao, Narasimhan, Press) argues that a language-model agent is a new
kind of computer user that needs an interface designed for it, the way developers benefit from an IDE. The
paper reports 12.5% pass@1 on SWE-bench and 87.7% on HumanEvalFix with the model of the time (V, abstract page).
The headline is not the number but the claim: **the interface, not just the model, determines the result**.

### 2.2 The interface (V, paper HTML and docs)

- Design principles stated in the paper: keep actions simple and easy to understand; make actions compact and
  efficient (consolidate operations into few steps); give informative but concise feedback; add guardrails that
  limit error propagation and speed recovery.
- Commands: search `find_file`, `search_file`, `search_dir`; viewer `open`, `goto`, `scroll_up`,
  `scroll_down`; editing `edit` (replace a line range in the open file), `create`; and `submit`.
- File viewer: at most 100 lines per turn. The docs say this size worked best; the ablation below agrees.
- Search output: `search_dir` lists only the files that contain a match; the docs say showing more context per
  match confused the model. Search returns at most 50 results; above that nothing is shown and the model is
  asked to write a more specific query (a deliberate refusal to dump).
- Empty output: an explicit sentence ("Your command ran successfully and did not produce any output") so that
  silence is never ambiguous.
- Linter on edit: the edit is checked with flake8 for syntax and undefined names. If it introduces an error the
  edit is **reverted**, and the model gets the error, a preview of what it tried and the original content.

### 2.3 Ablation (V, SWE-bench Lite, full system 18.0% resolved, paper table)

| Component | Variant | Resolved | Change |
|---|---|---|---|
| Edit | no edit command | 10.3% | -7.7 |
| Edit | without linting | 15.0% | -3.0 |
| Search | iterative search (open one result at a time) | 12.0% | -6.0 |
| Search | no search | 15.7% | -2.3 |
| Viewer | 30 lines | 14.3% | -3.7 |
| Viewer | whole file | 12.7% | -5.3 |
| Context | full history | 15.0% | -3.0 |
| Context | without the demonstration | 16.3% | -1.7 |

Reading: (1) the **summarised search** (files only, capped) beats both no search and iterative search; the badly
designed search (iterative) is worse than none. (2) Linting is worth 3 points: cheap deterministic verification
at the moment of the action. (3) A whole file in the viewer is worse than 100 lines, and 30 lines is also worse:
the observation size has an optimum. (4) **Collapsing old observations** ("context" row: full history is worse
by 3 points) helps. The paper keeps the latest observations whole and collapses older ones to one line each (R:
that is the mechanism; the table above is V).

### 2.4 Tool choice, state, verification

- Tool choice: the model, each step, in a thought-then-action format. There is no router. The interface limits
  the damage a poor choice can do (caps, reverts) instead of preventing it.
- State: the trajectory (message history) with collapsing of old observations. No structured investigation
  state, no list of "files already seen" other than what the viewer prints (it shows the open file and current
  line). (R for the absence; V for the viewer state.)
- Verification: the linter on every edit; the model is expected to write and run reproduction scripts itself
  (R).
- Later work: the SWE-agent site says the project is in maintenance mode and recommends mini-swe-agent (V).
  mini-swe-agent is about 100 lines of agent code, **bash as the only tool**, no tool-calling interface, linear
  history, each action an independent subprocess (V, README). It claims above 74% on SWE-bench Verified; this is
  the project's own claim, not checked by me (V that the README claims it, not that it is true).

### 2.5 Pre-LLM stage and other sources

- Pre-LLM stage: none. The issue text goes straight to the model. The "context" is whatever the model fetches.
- Other sources: none; the agent lives in a repository sandbox.

### 2.6 Take, adapt, reject

- TAKE the observation discipline: caps with a refusal message that tells the model how to narrow
  ("too many results, be more specific"), explicit empty-output sentences, per-file listing before per-line
  context, a viewer window instead of whole files. katana's Grep/Find/Read already move this way; the UIOE
  should apply one **observation contract** to every source (section 14, lesson 5).
- TAKE linter-on-edit as a verification layer owned by the engine, not by the model (katana has LSP and
  tree-sitter-like parsers for the index; a syntax check before an `Edit` lands is deterministic and cheap).
- ADAPT collapsing of old observations: with a 1 to 2 minute step the agent will have few steps, so the point is
  not cost but keeping the model's attention on the evidence packet (section 16).
- REJECT iterative one-result-at-a-time search as the default; the ablation shows it is worse than no search.
  The UIOE replaces it with a ranked, deduplicated packet.
- REJECT "bash only" as the access method for a small local model: it works for strong models that already know
  the shell idioms (R); the evidence for small models is the opposite (section 17).

## 3. Agentless (localization in levels, no agent)

Sources: arXiv:2407.01489 (abstract page and HTML).

### 3.1 The design (V)

Agentless is a three-phase pipeline (localize, repair, validate) in which the **program, not the model, decides
what happens next**. On SWE-bench Lite the paper reports 32.00% (96 of 300) at about $0.70 per issue, the
highest result and lowest cost among open-source agents at the time of the paper, with GPT-4o
(gpt-4o-2024-05-13) (V). It also audits the benchmark and builds a cleaned subset, Lite-S.

Hierarchical localization:

1. File level. Two independent methods are combined.
   - Prompting: a repository tree in the style of the Linux `tree` command goes to the model, which ranks
     suspicious files (default top 3).
   - Embedding retrieval: the model first flags irrelevant folders, those files are removed, the rest is
     chunked (512 tokens, no overlap), embedded (text-embedding-3-small via LlamaIndex) and ranked by cosine
     similarity to the issue. The final list keeps files both methods agree on.
2. Related element level. Each suspicious file is reduced to a **skeleton**: class and function headers, class
   fields, method signatures, class and module comments. The model chooses the related classes and functions.
3. Edit location level. The complete code of just those elements goes to the model, which names edit locations
   (line, function or class). Four independent samples of locations per issue.

Repair and validation:

- Context ±10 lines around each location, Search/Replace diff format, 10 patches per location set
  (1 greedy, 9 sampled), 40 patches per bug.
- Reproduction tests: 40 candidates; only tests that print "Issue reproduced" on the unmodified repo are kept;
  the most frequent normalised test is selected. Regression tests: the existing suite, minus tests the model
  judges may need updating. Patches are filtered by fewest regression failures, then by passing the
  reproduction test; majority vote over AST-normalised patches picks the winner.

### 3.2 Numbers worth keeping (V, Table 2 to 4 of the paper)

| Step | Variant | Ground truth contained | Avg lines shown | Avg cost |
|---|---|---|---|---|
| file | prompting only | 78.67% | 3,221 | $0.02 |
| file | embedding only (with folder filter) | 70.33% | 3,622 | $0.04 |
| file | combined | 81.67% | 3,424 | $0.06 |
| element | whole file | 53.67% | 778 | $0.15 |
| element | skeleton | 58.33% | 698 | $0.02 |
| edit location | straight from file level | 47.00% | 208 | $0.18 |
| edit location | via element level (hierarchy) | 49.67% to 56.33% | 165 to 342 | $0.07 |

- The skeleton is **both cheaper and more accurate than the whole file** (58.33% vs 53.67%; $0.02 vs $0.15).
- Going directly from file level to lines is worse than passing through the element level: **the hierarchy
  itself helps**.
- Two different retrievers, combined, beat each alone at file level (81.67% vs 78.67% and 70.33%).
- Validation: majority vote alone fixes 77, adding regression tests 81, adding reproduction tests 96. The
  reproduction test is the largest single contribution (V). Of 300 issues, 213 yield a reproducing test, and only
  94 of those print "resolved" under the reference patch, so vague issues limit this step (V).
- Performance plateaus around 40 patch samples (V).

### 3.3 Answers to the six questions

- (a) Observations: three fixed shapes (tree, skeleton, element code), sized by construction.
- (b) Routing: **fixed by the program**. No tool choice at all. This is the closest match to the UIOE idea.
- (c) State: the pipeline's intermediate lists (files, elements, locations). Explicit, inspectable.
- (d) Verification: generated reproduction tests plus regression tests, with voting.
- (g) Pre-LLM stage: the pipeline is the router; the model is a component called with a prepared input at each
  level. Note the embedding retriever is an input to context building, not the model.
- (h) Context builder: tree, then skeleton, then full code of selected elements; sizes shrink at each level.
- (i) Other sources: none.

### 3.4 Take, adapt, reject

- TAKE the **three-level descent** (files, then skeleton of elements, then bodies) as the shape of katana's
  evidence packet. katana's N0/N1/N2 levels and the packet's hypothesis-then-excerpt structure already follow
  it; the UIOE should make the descent explicit and budgeted per level.
- TAKE the **skeleton** as the default view of any file that is not the target: signatures and comments, no
  bodies. This is also Aider's idea (section 5). It is a deterministic transform, a good fit.
- TAKE "two independent retrievers, keep the agreement": katana's CSSE has five searches plus a ranker; the
  UIOE should record, per candidate, how many independent routes found it (an explainable confidence).
- ADAPT folder filtering ("which folders are irrelevant"): do it with rules (vendor, generated, tests,
  ignored with reason, already in katana's lifecycle doc) before asking any model.
- REJECT embeddings (constraint). REJECT the sampling-and-voting repair stage: 40 patches is incompatible with
  1 to 2 minutes per step. Keep the principle, not the machinery: **reproduction before patch** is the best
  verification the paper found; for katana it means the investigation should end with a concrete check the
  model can run (a test, a command), proposed by the engine when it can.

## 4. AutoCodeRover (search by program structure)

Sources: arXiv:2404.05427 (abstract and HTML).

### 4.1 Design (V)

- Treats software as classes and methods in an AST rather than files. Reports 19% on SWE-bench Lite at about
  $0.43 per issue in the abstract (V); the paper's table lists 57 resolved (19%) without fault localization,
  66 (22%) with spectrum-based fault localization (SBFL), 78 (26%) with three runs (V).
- Search API (V, Table 1): `search_class`, `search_class_in_file`, `search_method`, `search_method_in_class`,
  `search_method_in_file`, `search_code` (snippet with ±3 lines), `search_code_in_file`.
- Outputs are deliberately short: a class search returns only the **signature**, a method search the
  implementation, a code search ±3 lines. The paper argues signatures are better than truncating at a fixed
  bound. An unimplemented API call returns an error message to the model.
- Stratified search: stratum 1 context is only the issue text; later strata add what was retrieved. In each
  stratum the model picks the API calls it needs (several at once) and then decides whether the context is
  sufficient. Capped at ten rounds.
- Two agents: retrieval, then patch writing; the patch agent retries up to three times with a Python syntax
  linter.
- SBFL, only when a test suite exists: top 5 suspicious methods are passed to the agent as "results of an
  external analysis tool", which the agent cross-checks against the issue text (V).

### 4.2 Answers

- (a) short, typed results (signature vs body vs snippet).
- (b) the model chooses API calls, but inside a **closed, structure-aware vocabulary**.
- (c) the accumulated context across strata; the "is it enough?" decision is explicit.
- (d) test-based SBFL and the retry loop with a linter.
- (g) none before the model, except SBFL when tests exist (that is a pre-LLM analysis step feeding the prompt).
- (h) context = problem statement plus API results, accumulated.

### 4.3 Take, adapt, reject

- TAKE the **API vocabulary by structure** (class signature, method body in class, snippet with ±3 lines). It maps
  one-to-one to katana's `Symbols` actions (def, outline, refs). The UIOE should expose the same closed
  vocabulary internally and use it from rules.
- TAKE "signature for a class, body for a method" as a default size policy.
- TAKE the explicit **sufficiency check**: after each round, a decision "enough / not enough" with a stated
  reason. In katana this is the gap list ("Not found") and the confidence grades.
- ADAPT SBFL: katana has no test coverage data in general; but where a failing test or stack trace is in the
  request, the engine can turn it into an exact entry point (a trace gives file and line). That is the cheap
  deterministic cousin of SBFL.
- REJECT the model-driven ten-round loop at 1 to 2 minutes per round; keep one engine-driven round and let the
  model ask for a second.

## 5. Aider (repo map)

Sources: aider.chat repo map article (2023-10-22), the repo-map docs page, and `aider/repomap.py` on GitHub.

### 5.1 What goes to the model (V)

- A concise map of the repository: files and, for each, the key symbols with the **critical source lines**
  (class declarations, function signatures), printed with a `│` prefix and `⋮...` for omitted code.
- It is sent with every change request so the model can reuse existing abstractions.
- Budget: `--map-tokens`, default 1k tokens. The map is dynamic: the docs say it can expand a lot when no files
  are in the chat and the model needs a wider view.
- Only identifiers that are most referenced elsewhere are shown, not everything.

### 5.2 How it is ranked (V, from the source file)

- Tags (definitions and references) come from tree-sitter queries; if a file has definitions but no
  references, a lexer backfills references. Tags are cached per file keyed by mtime.
- A graph is built: an edge runs from each referencing file to each defining file for a shared identifier;
  definitions with no references get a small self-edge (0.1).
- Edge weight = multiplier times the square root of the reference count (the square root damps frequent names).
  Multipliers (as read in the source): identifier mentioned in the user text x10; a long (8+ characters)
  snake/kebab/camel case name x10; a private name with leading underscore x0.1; a name defined in more than 5
  files x0.1; the referencing file is in the chat x50.
- PageRank is **personalised**: files in the chat and files or identifiers mentioned in the user message get
  the personalisation weight. Fallback if the computation fails: retry without personalisation, else empty.
- Fitting the budget: binary search on the number of tags rendered, accepting a result within 15% of the
  budget; token counts are estimated by sampling on large text.
- Caching at several levels (tags, rendered snippets, whole map) with refresh modes (`auto` reuses the cache
  only if the last computation took more than one second; `manual`; `always`; `files`).

### 5.3 Answers

- (a) a compact, stable text map; the edit protocol is separate (diffs in the model's reply).
- (b) **No tool choice for navigation at all**: the map is always given; the user adds files to the chat.
  The model asks for files by name and the tool adds them (R, from usage).
- (c) state = the set of files in the chat plus chat history; the map is recomputed around them.
- (d) optional lint and test commands after edits, with the output fed back (R).
- (g) **Yes, a pure pre-LLM context builder**: parse, graph, personalised rank, budgeted rendering, before every
  request.
- (h) selection by rank, ordering by rank then file, budget by binary search, summarisation by "signature
  lines only", citation implicit (file path headers).
- (i) other sources: not in the map. `/web` fetches a page into the chat on demand (R).

### 5.4 Take, adapt, reject

- TAKE the **personalised graph rank with a token budget** as the UIOE's method to choose which part of the
  code index to show when there is no specific entry point (overview or vague requests). It is deterministic,
  has no embeddings and is explainable: "shown because referenced from N files and the request mentions X".
  The weights above are a ready-made starting set; the sqrt damping and the "defined in many files" penalty
  are the pieces most likely to transfer.
- TAKE the personalisation by request terms: katana's interpreter already extracts terms with document
  frequency; use them as the PageRank restart vector.
- ADAPT to katana's richer graph: Aider's graph is file to file by shared identifier; katana has resolved
  calls and links by text key, so edges can be symbol to symbol with grades (exact, derived, probable) and
  the rank can weight exact edges more.
- ADAPT the budget loop: katana's per-hypothesis token shares (S7) already do this; add Aider's cache key idea
  (inputs: chat files, budget, mentioned terms) so repeated steps in one investigation reuse the map.
- REJECT "the user adds files to the chat" as the state model; the UIOE keeps investigation state itself.

## 6. OpenHands (CodeAct, runtime, condenser)

Sources: paper arXiv:2407.16741, CodeAct arXiv:2402.01030, OpenHands runtime docs, SDK condenser docs and blog,
arXiv:2508.21433 (JetBrains).

### 6.1 Design (V unless marked)

- OpenHands (formerly OpenDevin) is an open platform for agents that write code, use a command line and browse
  the web; MIT licensed; evaluated on 15 tasks including SWE-bench and WebArena (V, abstract).
- CodeAct: the agent's actions are **executable Python code** instead of fixed JSON; the paper reports up to 20%
  higher success than alternatives on its benchmarks, 17 models, and releases a 7k-interaction instruction
  dataset (V, abstract). For small models the relevant lesson is in section 17.
- Runtime: a Docker container holds an action-execution server (bash shell, optional Jupyter and browser). An
  **event stream** carries Actions from the agent and Observations back, over REST (V, docs). The cycle is
  agent, action, sandbox execution, observation, agent.
- Condenser: the SDK docs list an LLM-summarising condenser as the default; it summarises history after a
  size limit and keeps the latest messages intact (V, search snippet of the docs). An observation-masking
  condenser (replace observations outside a window with a placeholder) also exists (R/V mixed: V that
  masking is described by the JetBrains paper, R for the exact class name).
- Evidence on condensation (V, abstract of arXiv:2508.21433): in SWE-agent on SWE-bench Verified across five
  model configurations, **simple observation masking cut cost about in half** versus an unmanaged agent with
  solve rates comparable to LLM summarisation (occasionally slightly higher); a hybrid cut a further 7% versus
  masking and 11% versus summarisation; preliminary results held on OpenHands.

### 6.2 Answers

- (a) observations are action-typed events (command output, file contents, browser state); size is controlled
  by truncation in the runtime and by the condenser at the history level (R for truncation details).
- (b) model chooses (CodeAct); no router.
- (c) the event stream is the state; the condenser trims it. There is a microagent mechanism that injects
  repository instructions and keyword-triggered knowledge (R).
- (d) tests and commands run in the sandbox.
- (g) pre-LLM stage: none for retrieval; microagents are a keyword-triggered context injection (R), which is a
  rule-based pre-LLM step in miniature.
- (i) other sources: browser and web via actions; MCP via tools (R).

### 6.3 Take, adapt, reject

- TAKE the **event-stream model** (typed Action and Observation records) as the shape of the UIOE's
  investigation log: every source call is a typed event with source, query, result summary and grade. It makes
  the investigation replayable and explainable, and it is what a context builder needs to dedupe.
- TAKE observation masking over LLM summarisation for history. It is deterministic (no model call, which costs
  1 to 2 minutes), explainable and measured at half the cost with equal solve rate (V).
- ADAPT keyword-triggered microagents into **rule-triggered source adapters**: "request mentions a ticker, query
  the finance source" is the same mechanism in the general UIOE.
- REJECT CodeAct as the action language for a small local model, and LLM-summarising condensers (a model call
  per condensation, non-deterministic).

## 7. Claude Code (Read, Grep, Glob, subagents, memory, compaction)

Sources: Claude Code documentation pages (subagents, memory, best practices), Anthropic engineering post on
writing tools for agents. Public documentation only; internal prompts are not claimed.

### 7.1 What the docs state (V)

- Subagents run in **their own context window** with a custom system prompt, restricted tool allowlist or
  denylist and independent permissions; they return **only a summary** to the parent. Definition files are
  Markdown with YAML frontmatter (name, description, tools, model). Delegation is automatic from the
  `description`, or explicit by name.
- A built-in **Explore** subagent is read-only (Write and Edit denied), searches and analyses code, with
  thoroughness levels `quick`, `medium`, `very thorough`.
- Memory: `CLAUDE.md` files (project, user, organisation scopes; `AGENTS.md` can be read in its place; path-scoped
  rules in `.claude/rules/` load only when matching files are touched). Imports organise a long file but do not
  reduce its context cost. A separate **auto memory** (notes the model writes itself) loads the first 200 lines
  or 25KB of `MEMORY.md`. Each session starts with a fresh context window; these files carry knowledge across.
- Best-practices page: the conversation is **compacted automatically** before the window fills; the page's
  thesis is "tell Claude what it can't see, give it a check to run" (verification), and enforce non-negotiable
  rules with hooks rather than prose.
- Tool design (Anthropic engineering): few, high-impact tools instead of wrapping every endpoint ("too many or
  overlapping tools can distract agents"); namespacing; return meaningful names rather than opaque IDs; a
  `response_format` enum (concise or detailed; in their Slack example the concise reply used 72 tokens versus
  206); **default limits** on large responses (Claude Code caps tool responses at 25,000 tokens by default);
  when truncating, tell the model how to narrow; error messages that teach the correct call; evaluate with
  realistic multi-call tasks and track tool-call counts.
- Claude Code's search for code is Glob and Grep tools driven by the model (no embedding index); that this is
  a deliberate choice, which its authors have discussed in interviews, is **R**, not checked here.

### 7.2 Answers

- (a) tool results are plain text with caps (25k tokens by default); file reads with offsets and limits (R).
- (b) the model chooses tools; subagents give it a way to delegate exploration; descriptions are the routing
  signal for subagents.
- (c) state: conversation plus compaction summaries, plus memory files, plus a todo list (R).
- (d) the docs push "give a check to run": tests, linters, hooks.
- (g) pre-LLM stage: **memory files and rules are injected before the first turn**; path-scoped rules are a
  rule-based pre-LLM selection. Retrieval of code itself is not pre-LLM.
- (h) context builder: system prompt + memory + user message, then tool results as they come; compaction is the
  budget mechanism.
- (i) other sources: MCP servers as tools; WebFetch and WebSearch tools; subagents restricted per MCP server
  (V for subagent MCP control).

### 7.3 Take, adapt, reject

- TAKE the **read-only exploration subagent returning a summary**: the pattern separates the noisy search
  trace from the main context. For katana the UIOE itself plays the Explore role, with the trace kept in its
  own log and only the packet reaching the model. (katana already has `Agent` and `SubagentHandback`.)
- TAKE **path-scoped rules loaded only when matching files are touched**: a deterministic, explainable
  context-builder rule for project conventions (katana has CLAUDE.md-style files already).
- TAKE the tool-design guidance wholesale for the UIOE's source adapters: concise and detailed response
  formats, hard defaults, truncation messages that say how to narrow, actionable errors.
- ADAPT compaction to deterministic masking (section 6) and a structured investigation state that is
  re-emitted rather than summarised.
- REJECT relying on the model to pick among many tools. katana's own `docs/tools.md` records a full tool set
  costing prompt tokens (8.8k characters short form). For a small model every tool in the prompt is a cost
  and a chance of wrong choice; the UIOE should present **one** retrieval entry point plus a few escape
  hatches.

## 8. OpenAI Codex CLI and AGENTS.md

Sources: github.com/openai/codex README, agents.md.

- V: Codex CLI is an open-source (Apache-2.0) coding agent that runs locally in the terminal. The README does
  not describe tools, sandbox or approvals; I did not read the developer docs, so everything below about its
  tool set is **R**.
- V: AGENTS.md is a plain-Markdown "README for agents" with no required fields; the nearest file in the tree
  to the edited file wins; explicit user prompts override; if test or lint commands are listed, agents run
  them before finishing; supported by several tools (Codex, Aider, Cursor, VS Code and others).
- R: Codex uses a shell tool and a patch-apply tool, runs commands inside an OS sandbox with approval modes,
  and relies on the model to search with shell commands such as ripgrep.

Answers: (a) command output, truncated by the runtime (R); (b) model; (c) conversation and AGENTS.md;
(d) the commands listed in AGENTS.md; (g) AGENTS.md concatenation is the only pre-LLM context step (V for the
nearest-file rule).

Take, adapt, reject: TAKE the AGENTS.md convention as an input of the UIOE (katana already reads CLAUDE.md;
supporting AGENTS.md costs nothing and the nearest-file precedence is a deterministic rule). TAKE "commands to
verify are declared by the project" as a source of verification checks. REJECT the shell-only retrieval for
the same reason as mini-swe-agent.

## 9. Cursor and Windsurf/Devin

Sources: Cursor documentation (codebase indexing page), Cursor blog "Securely indexing large codebases"
(Jeremy Stribling, dated 2026-01-27 on the page), Windsurf docs (via search results and redirect to Devin docs).

### 9.1 Cursor (V)

- Indexing pipeline: every file is hashed, every folder hash derives from its children (a **Merkle tree**);
  comparing client and server trees syncs only divergent branches; for a 50,000-file workspace the file names
  and SHA-256 hashes total about 3.2 MB. Changed files are split into syntactic chunks and embedded
  asynchronously; embeddings are cached by chunk content so unchanged chunks are not recomputed.
- Reuse across teammates: the client derives a **simhash** from its tree, the server finds a similar existing
  index in a vector database of simhashes and reuses it. To prevent leakage the client must upload its whole
  tree as proof, and results for files it cannot prove are dropped. Clones of the same repo average 92%
  similarity across users in an organisation. Time to first query: median 7.87 s to 525 ms; p90 2.82 min to
  1.87 s; p99 4.03 h to 21 s. Semantic search becomes available at 80% indexed. The post states semantic search
  improved response accuracy by 12.5% on average (V: it says so; I did not see the methodology).
- A separate documentation page describes **Instant Grep**: a custom search engine that the page says
  outperforms ripgrep on large codebases, built and queried on the user's machine (V). The page also describes an
  Explore subagent with its own context window and a faster model that runs parallel searches and returns
  summarised findings (V).
- Rules (R): `.cursor/rules` files attached by globs or always; the model sees the rule text when matching
  files are in context.

### 9.2 Windsurf (V from search result snippets of official docs, not full pages)

- Windsurf describes a RAG-based context engine using its own "M-Query" techniques. **Fast Context** is a
  subagent that retrieves code up to 20x faster than ordinary agentic search, using SWE-grep models that run up
  to 8 parallel tool calls per turn for a maximum of 4 turns (announced October 2025 per the coverage found).
  Third-party claims about embedding sizes and thresholds were not confirmed and are not used here.
- Devin: no first-hand material read; Devin's "DeepWiki"/indexing and playbooks are **R** only.

### 9.3 Answers

- (a) retrieved chunks and file excerpts, returned by search tools or injected; sizes are tool defaults.
- (b) hybrid: the model calls grep or semantic search; the product adds retrieval around it. A trained small
  search model (SWE-grep) is a **specialised retriever subagent** (V that it exists; details R).
- (c) the index is persistent state; conversation state is separate.
- (g) pre-LLM stage: **yes**: attached files, rules, open tabs and semantic retrieval are assembled before the
  model sees the turn (V for retrieval and rules existing; R for the assembly order).
- (h) the builder selects by recency and similarity (R); budgets by window.
- (i) @-mentions for docs, web and git, and MCP (R).

### 9.4 Take, adapt, reject

- TAKE the **Merkle tree for change detection**: katana's lifecycle doc has a crawler that re-walks levels and
  watches with inotify; a hash tree over folders makes "is this subtree unchanged?" a constant-time check and
  gives each evidence line a verifiable freshness. This is deterministic and needs no embeddings.
- TAKE the idea of a **fast specialised search path with bounded turns** (4 turns, parallel calls): the engine
  runs its fixed routes in parallel inside one UIOE call.
- ADAPT the cache-by-content idea: key derived results (summaries, tree-sitter parses) by content hash.
- REJECT embeddings and the simhash-sharing of vector indexes (needs a server; katana's constraint is no
  service in the core). The accuracy gain (12.5% per Cursor) is the figure katana's CSSE has to earn by other
  means; it is a reminder that lexical-only retrieval has a measured gap on conceptual queries.

## 10. Cody/Sourcegraph and Amp

Sources: Sourcegraph Cody context documentation; Amp is **R** only.

- V: Cody retrieves codebase context from three sources: **keyword search** (queries sometimes rewritten to add
  relevant terms), **Sourcegraph Search** (the instance's search engine, returning documents to the IDE) and
  the **code graph** (components and their usage relationships). Users add context with @-mentions: files,
  symbols, web URLs, remote files and directories; OpenCtx sources are available in some clients. Multiple
  linked repositories are supported. A prompt has three parts: optional prefix, user input, grounding context.
  The context window has an administrator-adjustable limit; the page notes a smaller window can produce
  "selected too much code" errors.
- R: Cody historically also used embeddings and moved to search-based retrieval; Sourcegraph's Amp is an agent
  using search subagents over its code intelligence (SCIP-based precise code navigation).

Answers: (a) ranked snippets with file paths, trimmed to the window; (b) the **retrieval is a pipeline
decided by the product**, with user @-mentions as an override; (c) conversation; (g) **yes: the strongest
example of a pre-LLM context builder**: interpret, rewrite query, fetch from several retrievers, rank, fit
window, then call the model; (h) selection by retriever scores, user-mentioned items first (R for ordering),
budget by window, citation by showing the files used (R); (i) other sources: web URLs, remote repos, OpenCtx
providers (docs, issue trackers) (V for OpenCtx existing; R for provider list).

Take, adapt, reject: TAKE the three-retriever fusion (keyword, structured search, code graph), user
@-mentions as highest-priority evidence, query rewriting by rules (synonyms, identifier splitting; katana's
CSSE knowledge base does this), and the **code graph as an independent retriever** whose agreement with lexical
hits raises confidence. TAKE the **provider interface** (OpenCtx idea: every source answers "give me items
for this query" with title, URL, snippet) as the template for the UIOE source adapter. REJECT dependence on a
hosted search service.

## 11. Cline, Roo, Continue

Sources: Cline plan/act documentation, Continue context-provider documentation. The tool names of Cline
below are R.

### 11.1 Cline and Roo (R unless marked)

- V: Cline has Plan mode (reads and searches, cannot modify or execute) and Act mode (modifies and executes);
  switching keeps the history; separate models per mode; `/deep-planning` has Cline explore the codebase
  systematically, identify affected files and dependencies, produce a plan and ask clarifying questions before
  acting (V). Checkpoints let the user roll back (V).
- R: its exploration tools are `list_files`, `search_files` (regex), `list_code_definition_names` (a
  tree-sitter outline of a directory) and `read_file`; edits via a replace-in-file diff tool; results come in
  XML-tagged tool messages; a Memory Bank convention (markdown files the agent maintains) carries state
  across sessions; Roo adds more modes and custom instructions per mode. Context is truncated by dropping
  middle messages when the window fills.
- Pre-LLM stage: environment details block (open tabs, file tree) attached to each turn (R). Other sources: MCP
  servers are a first-class tool category (R).

### 11.2 Continue (V from docs page)

- Context providers are explicit @-items: @File, @Terminal, @Code, @Git Diff, @Current File, @Open, @Clipboard,
  @Tree, @Problems, @Debugger, @Repository Map (files plus call signatures of top-level classes, functions,
  methods), @HTTP; any MCP server can be added. The older @Codebase, @Docs, @Url and @Web are marked deprecated
  in favour of MCP servers (V). How codebase retrieval worked (embeddings plus keyword search plus reranking)
  is **R**.

### 11.3 Take, adapt, reject

- TAKE the **Plan versus Act separation as a permission boundary**: katana already has `EnterPlanMode`; the UIOE
  should run only read-only sources, which makes it safe by construction.
- TAKE "clarifying questions before acting" as a UIOE output: when the interpreter cannot classify (type NONE or
  several hypotheses with similar scores), return the question instead of a guess; katana has `AskUserQuestion`.
- TAKE Continue's **context provider list** as a checklist of source adapters a general UIOE needs (diff, open
  files, problems from LSP, terminal output, tree, repository map, HTTP, MCP).
- ADAPT Memory Bank into an investigation store that is machine-written, structured and versioned, not a pile
  of prose the small model must re-read.
- REJECT per-turn "environment details" dumps (open tabs, whole tree): the cost is paid every step.

## 12. Graph and retrieval research for localization

Sources: arXiv abstract pages and HTML for CodexGraph (2408.03910), LocAgent (2503.09089), RepoGraph
(2410.14684), LingmaAgent (2406.01422), RepoCoder (2303.12570, via search results), Moatless Tools (GitHub and
package listing). CodeRAG is **R** only; I did not retrieve a source.

### 12.1 LocAgent (V)

- Parses a repository into a directed heterogeneous graph: node types directory, file, class, function; edge
  types contain, import, invoke, inherit. Three tools only: `SearchEntity` (keyword to entities with snippets at
  fold, preview or full detail), `TraverseGraph` (type-aware breadth-first search with direction, hop count,
  entity and relation filters) and `RetrieveEntity`. The index is a sparse hierarchical one: entity-ID index,
  name-to-node dictionary, **BM25** over entity IDs and a chunk-to-entity index. No embeddings in the retrieval
  index as described.
- Reported (V): file-level Acc@5 on SWE-Bench-Lite: fine-tuned Qwen2.5-7B 88.32%, fine-tuned Qwen2.5-32B
  92.70%, Claude-3.5 94.16%; on Loc-Bench: 7B 78.57%, Claude-3.5 83.39%; the 32B cost about $0.09 per example
  against $0.66 for Claude-3.5; removing the BM25 index lowered the 7B model's SWE-Bench-Lite file Acc@5 from
  88.32% to 75.18%. The abstract adds roughly 86% cost reduction and a 12% improvement in issue resolution at
  pass@10.
- Relevance: **small open models, fine-tuned for a narrow tool vocabulary over a code graph with lexical
  indexing, matched a frontier model on localization.** This is the closest published precedent to katana's
  bet (small local model, lexical plus graph, no embeddings).

### 12.2 CodexGraph (V)

- Code graph in a Neo4j database queried with Cypher; schema nodes MODULE, CLASS, METHOD, FUNCTION, FIELD,
  GLOBAL_VARIABLE and edges CONTAINS, INHERITS, HAS_METHOD, HAS_FIELD, USES. A **primary agent writes the
  question in natural language and a separate translation agent writes the Cypher**; removing the translation
  agent dropped GPT-4o's CrossCodeEval Lite exact match from 27.90 to 8.30.
- SWE-bench Lite pass@1 22.96 with GPT-4o (equal to AutoCodeRover's number in the same table) versus BM25 at
  3.11; but it spends about 102k tokens per SWE-bench task versus 14.76k for BM25.
- **Weak models fail at it**: with Qwen2-72B-Instruct the CrossCodeEval exact match was 5.00, below no
  retrieval (8.20) and BM25 (15.50), because the model could not write valid graph queries (V).
- Lesson: letting a model write a free-form query language is the failure mode for small models. A closed
  vocabulary of precomputed questions (what katana's `Symbols` actions are) is the safe form.

### 12.3 RepoGraph, LingmaAgent, RepoCoder, Moatless

- RepoGraph (V, abstract): a plug-in repository-level code graph module that improved all four SWE-bench
  methods it was plugged into, plus CrossCodeEval; ICLR 2025. No numbers read; the abstract gives none.
- LingmaAgent (V, abstract): condenses repository knowledge into a graph and uses Monte Carlo tree search to
  explore; reports 18.5% relative improvement over SWE-agent on SWE-bench Lite and 16.9% of in-house issues
  resolved automatically (43.3% with manual intervention). The tree search is expensive for katana: REJECT.
- RepoCoder (V, search result of abstract): iterative retrieval-generation, using the previous completion as the
  next retrieval query; over 10% better than the zero-shot baseline in all settings it tested. The lesson:
  **the draft answer is a better query than the question**. For katana: after the first packet, the model's
  first hypothesis can be fed back as terms for a second cheap lexical round.
- Moatless Tools (V from README listing and a search snippet): "build good tools to insert the right context
  into the prompt and handle the response", a code index for semantic search plus actions for finding classes,
  functions and code snippets, and a file context object that tracks what the model has seen and what changed;
  the semantic index needs an embedding API. The numbers found (24% at $0.13 on an older README, 70.8% at
  $0.63 for a later listing) are not comparable and are version-specific. TAKE the **FileContext idea**: a
  structured record of which spans the model has seen, so the engine never re-sends them (section 14, lesson 7).
- CodeRAG, HyperAgent (V: planner, navigator, editor, executor multi-agent, abstract only), Agentless
  descendants: not studied in depth.

### 12.4 Take, adapt, reject (graphs)

- TAKE LocAgent's three-tool shape: search, traverse (typed BFS, bounded hops) and retrieve (by ID). It
  matches katana's Symbols (def, callers, callees, deps, impact) and shows the vocabulary can be tiny.
- TAKE BM25 over entity names plus a name-to-node dictionary as the primary entry; the ablation (88.32 to
  75.18) shows the lexical index is load-bearing.
- TAKE the observation that fine-tuning a small model on the tool vocabulary closes the gap (R for how katana
  would train; `docs/training-data.md` exists). Note katana's rule that only tools, planning and conventions
  go into training applies.
- REJECT free-form graph query generation by the model (CodexGraph with weak models, V).
- REJECT MCTS (LingmaAgent) and multi-sample voting.

## 13. Comparison table

Columns follow the six questions plus the pre-LLM/context-builder column requested on 2026-11-23. "Evidence" is
V or R for the row's main claims. PLM = pre-LLM stage.

| System | Observation to model | Who picks the strategy | Investigation state | Verification | Benchmark lesson (V where numbered) | PLM stage / context builder | Other sources | Evidence |
|---|---|---|---|---|---|---|---|---|
| SWE-agent | 100-line viewer, capped search (50), explicit empty message | model, each step | trajectory, old observations collapsed | linter reverts bad edits | linter +3 pts; summarised search beats none and iterative; 100 lines optimal | none | none | V |
| mini-swe-agent | raw bash output | model | linear message list | model-run | tiny agent, bash only, strong model | none | none | V (README) |
| Agentless | tree, skeleton, element code | **program** (fixed pipeline) | pipeline lists | repro tests + regression + vote | skeleton beats whole file; hierarchy helps; combined retrievers 81.67% file recall; repro test is the biggest gain | pipeline is the builder | none | V |
| AutoCodeRover | signatures, method bodies, +-3 lines | model over closed structural API | accumulated strata, sufficiency decision | SBFL, tests, syntax linter | structure-aware search; SBFL 19% to 22% | SBFL only | none | V |
| Aider | repo map (signature lines), 1k tokens default | none (map always given) | chat files | optional lint/test | n/a | **yes: tags, personalised PageRank, binary-search budget** | /web on demand (R) | V |
| OpenHands | action/observation events | model (CodeAct) | event stream + condenser | sandbox tests | masking halves cost at equal solve rate | microagent keyword triggers (R) | browser, MCP (R) | V/R |
| Claude Code | capped tool text (25k default) | model; subagents by description | conversation, compaction, memory files, todos | "give a check", hooks | explore in a subagent, return summary | memory and path-scoped rules before turn 1 | MCP, WebFetch, WebSearch | V (docs) |
| Codex CLI | command output | model | conversation + AGENTS.md | commands from AGENTS.md | n/a | AGENTS.md nearest file | MCP (R) | V/R |
| Cursor | chunks, grep results, rules | model + product retrieval | persistent index, conversation | n/a | semantic search +12.5% accuracy (vendor claim) | **yes: rules, attached files, retrieval** | @docs, @web, MCP (R) | V/R |
| Windsurf / Devin | retrieved snippets | model + Fast Context subagent | index + memories (R) | n/a | specialised search model, 4 turns x 8 parallel calls | **yes: indexing + retrieval** | MCP (R) | partial V |
| Cody | ranked snippets, mentions first (R) | **product pipeline** + @-mentions | conversation | n/a | n/a | **yes: rewrite, three retrievers, window fit** | URLs, remote repos, OpenCtx | V |
| Cline/Roo | XML tool results, env details | model; Plan/Act modes | history, Memory Bank (R) | checkpoints | n/a | environment-details block (R) | MCP (R) | V/R |
| Continue | @-item contexts | **user** picks providers | chat | n/a | n/a | repository map provider, explicit mentions | MCP, HTTP | V |
| LocAgent | folded/preview/full entities | model over 3 tools | graph + visited set (R) | n/a | 7B-ft 88.32% vs Claude-3.5 94.16% file Acc@5; BM25 load-bearing | none | none | V |
| CodexGraph | query results from Neo4j | model writes queries | rounds | n/a | fails with weak models; 7x tokens vs BM25 | none | none | V |
| RepoCoder | retrieved snippets | fixed loop | previous draft | n/a | draft-as-query, +10% over zero-shot | loop builder | none | V (abstract) |
| katana (today) | evidence packet graded exact/derived/probable | **rules + one optional model call**, model keeps tools | CoderQuestion + case log | LSP confirm optional | none measured | **yes: S0 to S8** | web/MCP as tools only | local docs |

## 14. The ten lessons for the UIOE design

1. **Put the router in front of the model, and make it a program.** Agentless, Aider's map and Cody's pipeline
   all prepare the context before the model runs; the evidence is that program-chosen descent (files,
   skeletons, bodies) matched or beat model-driven agents at a fraction of the cost (V, Agentless: 32.00% at
   $0.70 with the strongest open-source result of its time). For a 1 to 2 minute step this is not an
   optimisation, it is the only affordable design. The model keeps a few tools as an escape hatch.
2. **Descend in levels and shrink at each level.** Tree, then skeleton, then bodies. The skeleton beat the
   whole file in both accuracy and cost (V: 58.33% vs 53.67%, $0.02 vs $0.15) and going straight to lines was
   worse than going through elements. The UIOE needs a level notion that is source-independent: catalogue of
   items, then summary per item, then full item.
3. **Fuse independent routes and expose the agreement.** Prompting plus embeddings beat each alone (V). The
   UIOE has no embeddings, but it has exact lookup, lexical search, graph traversal and path/name matching;
   count how many independent routes support each item and show that count as the explanation of confidence.
4. **Use a closed, typed vocabulary instead of free-form queries.** AutoCodeRover's structural API and
   LocAgent's three tools worked, including with fine-tuned 7B models (V); CodexGraph's free-form Cypher
   collapsed with a weak model (V). Internally the UIOE should be a small set of typed source calls
   (find, outline, traverse, fetch) with typed results.
5. **One observation contract for every source.** Cap, say how to narrow, never be silent when empty, give
   names not IDs, offer concise and detailed forms (V: SWE-agent caps and empty message; Anthropic tool
   guidance). A financial API and a code index return the same envelope: source, query, items with grade,
   freshness and citation, size, "not found", "truncated: narrow by X".
6. **Rank by a personalised graph and cut by a token budget.** Aider's map is a working recipe with published
   weights (V, source): personalise by request terms, damp with a square root, penalise ubiquitous names, boost
   what is already in focus, then binary-search the cut. It needs no model and explains itself in one line.
7. **Keep investigation state outside the model, as typed events.** OpenHands' event stream and Moatless'
   file context show the pattern; the UIOE should record every call and every span already shown, so that a
   second step does not re-send evidence, and so that the model's follow-up ("look at X") is resolved from
   state. State must be masked, not summarised: masking halves cost with equal solve rate (V, arXiv:2508.21433)
   and costs no model call.
8. **Verification belongs to the engine and comes before and after the model.** The linter revert (+3 points,
   V), reproduction test (largest single gain in Agentless, V), SBFL (+3 points on 300 issues, V) and
   Claude Code's "give it a check to run" (V) all say the same. In the UIOE: verify evidence (does the cited
   line still exist, is the file fresh: Merkle-style hash check), verify edits (syntax) and hand the model a
   check to run.
9. **Separate read-only investigation from action, and let uncertainty produce a question, not a guess.**
   Cline's Plan/Act (V), Claude Code's read-only Explore subagent (V) and Cline's clarifying questions (V) map
   to: the UIOE only reads; when the classifier is unsure it returns candidates or a question with the "Not
   found" section instead of a confident pack.
10. **Sources are adapters behind one interface, routed by rules that can be listed.** Cody's retrievers and
    OpenCtx providers, Continue's context providers and MCP show that "docs, web, issues, databases" fit one
    "query to items" interface (V for the existence of these; interface details R). Routing rules should be
    data (a table of triggers per source) so that the routing is explainable ("the request names a ticker, so
    the finance adapter ran") and trainable.

## 15. What these systems have that katana does not (yet)

Layers and functions seen in the studied systems and absent or only partial in katana, as far as I could see in
the three documents named at the top. Check each against the code before acting; I did not audit the source.

1. **A general source-adapter interface** (Cody/OpenCtx, Continue providers, MCP): katana's packet is code
   centred; web, MCP and files are model-called tools, not routed pre-LLM sources.
2. **A source router with listed triggers** that decides pre-LLM which of several domains (filesystem, web,
   finance, legal, database, code) to query. Partial: the interpreter types the question for code only.
3. **A cross-source evidence fusion step** with agreement counting (Agentless file-level fusion, Cody retrievers).
   katana fuses within code search (CSSE five searches); nothing fuses code with documents or web.
4. **A personalised graph rank with budgeted rendering for overviews** (Aider repo map). katana has per-hypothesis
   budget shares and a project card; a graph-ranked repo map for requests with no entry point is not described
   in the sections I read.
5. **Skeleton views as a first-class output** for non-target files (Agentless, Aider, Continue repository map).
6. **Investigation state as typed events with seen-span tracking** (OpenHands event stream, Moatless file
   context). katana logs the case but I did not see a "spans already shown to the model" register that the next
   step consults.
7. **Observation masking/collapsing of old results** (SWE-agent, JetBrains paper). Not described in the docs I read.
8. **Edit-time syntax verification with revert** (SWE-agent linter). katana has LSP and parsers; a mandatory
   check-and-revert on `Edit` may or may not exist (not verified).
9. **Test and reproduction awareness** (Agentless reproduction tests, SBFL, AGENTS.md declared commands): turning
   a stack trace or failing test in the request into an exact entry point, and proposing a check command.
10. **Hash-tree freshness** (Cursor Merkle tree) for constant-time "is this subtree unchanged" and per-line
    freshness proofs.
11. **Rule-triggered context injection** (OpenHands microagents, Claude Code path-scoped rules, Cursor rules):
    conventions loaded only when matching files or terms appear.
12. **A bounded, parallel retrieval subagent** (Windsurf Fast Context, Claude Code Explore, Cursor Explore):
    katana's UIOE would be this, with a fixed route set and a hard turn/time bound.
13. **Explicit sufficiency decision** (AutoCodeRover): "is the context enough" as a recorded output with a
    reason, driving whether a second round runs.
14. **Draft-as-query second round** (RepoCoder): feeding the first hypothesis back as retrieval terms.
15. **Response-format switch (concise/detailed) per call** (Anthropic tool guidance) for the model's follow-ups.
16. **A user-facing "what was included and why" view** (Cody's context list, Continue's explicit @items). katana
    writes a log; making the pack's provenance visible to the user is cheap trust.

## 16. Context builder design notes (what the studied builders do, for the UIOE's box)

This section distils item (h) across systems into the choices the UIOE's Context Builder must make.

- **Selection.** Aider: graph rank with personalisation. Cody: retriever scores with @-mentions first (R for
  ordering). Agentless: level-by-level model choice. Cursor/Windsurf: similarity plus recency (R). katana: hypotheses
  ranked, shares per hypothesis. Recommendation: rank by (route agreement, graph centrality personalised by the
  request, freshness), with explicit user mentions pinned.
- **Ordering.** The model reads the end of the context best (R, general long-context behaviour); katana appends
  the packet to the user turn right after the request (V from its own doc), which is consistent. Put the
  highest-grade evidence first within the packet and the "Not found" last, as katana does.
- **Budget.** Aider: binary search on rendered tokens within 15% (V). Claude Code: default tool-response cap of
  25,000 tokens (V). SWE-agent: 100 lines, 50 results (V). For a local small model the UIOE budget should be a
  small number set per source type with a global cap, and the builder must report what it dropped.
- **Summarisation.** Deterministic only: skeletons, signatures, first lines, per-directory summaries (katana
  N0 and N4), masking of old observations. No model summaries.
- **Citation.** Every item carries source, locator (path:lines, URL, record ID), grade and freshness. Cody lists
  the context used (V for context list existing is R; the page read does not state it). The model's answer
  can then be checked against the packet; this is also the training signal (the model learns to cite the
  locator).
- **Multi-domain.** Items from different sources need a common envelope and a common budget; unit of account is
  tokens, unit of fusion is the "topic hypothesis" (katana already groups candidates into hypotheses), which
  can span sources (a code symbol, a document paragraph and a database field about the same entity).

## 17. Evidence for small models specifically

Collected because it is the katana constraint:

- LocAgent: fine-tuned 7B reached 88.32% file Acc@5 on SWE-Bench-Lite versus 94.16% for Claude-3.5, with a
  three-tool vocabulary and BM25 (V).
- CodexGraph: a weak model (Qwen2-72B-Instruct) could not write graph queries and fell below no retrieval and
  BM25 on CrossCodeEval exact match (V).
- SWE-agent ablation: the interface matters more than any single component (V), and its authors moved to a
  bash-only agent with strong models later (V for the statement on the docs page).
- Anthropic: fewer, clearer tools; overlapping tools distract agents (V).
- Windsurf: a purpose-trained small search model with bounded turns was chosen over the general model for the
  search subtask (V from snippets; details R).
- Agentless: moving decisions out of the model into a pipeline works with a strong model; no data on a small
  model here (the paper uses GPT-4o).

What is not known (honest gaps): no source I read measured a 3B-active MoE running a pre-LLM context builder
versus a tool-calling loop. The inference that katana's design is favoured is an argument from cost (steps) and
from LocAgent/CodexGraph, not a measurement. katana's current rule is not to measure in this phase, so the
claim stays an argument.

## 18. Suggested next reading and open questions for the UIOE

1. Read the SWE-agent tool bundle source and Agentless prompts for the exact observation texts (not done here).
2. Read Aider `repomap.py` again when designing katana's symbol-level rank, since katana's graph is richer
   (calls and links): decide whether to rank symbols, files or both.
3. Decide the common evidence envelope (section 14, lesson 5) before writing any adapter.
4. Decide what the model may still call after the packet: the studied systems that work with small models keep
   the vocabulary tiny (three tools in LocAgent).
5. Check Codex CLI and Amp primary documentation (marked R here).
6. Check Devin/Windsurf indexing documents (only snippets read here).

## 19. Sources

All accessed 2026-11-23. "Read" means the page was fetched and summarised by the fetch tool in this session; a
fetch tool returns a summary, not the raw page, so quoted numbers are the tool's extraction and should be
re-checked at the URL before being cited in anything external.

Papers (arXiv):
- SWE-agent: https://arxiv.org/abs/2405.15793 and https://arxiv.org/html/2405.15793 (read the first 100,000
  characters of the HTML; the rest unread)
- Agentless: https://arxiv.org/abs/2407.01489 and https://arxiv.org/html/2407.01489 (first 100,000 characters)
- AutoCodeRover: https://arxiv.org/abs/2404.05427 and https://arxiv.org/html/2404.05427
- CodeAct: https://arxiv.org/abs/2402.01030
- OpenHands: https://arxiv.org/abs/2407.16741
- The Complexity Trap (observation masking vs summarisation): https://arxiv.org/abs/2508.21433
- RepoGraph: https://arxiv.org/abs/2410.14684
- LingmaAgent: https://arxiv.org/abs/2406.01422
- LocAgent: https://arxiv.org/abs/2503.09089 and https://arxiv.org/html/2503.09089
- CodexGraph: https://arxiv.org/html/2408.03910v2 (abstract details via search results at
  https://arxiv.org/abs/2408.03910)
- RepoCoder: https://arxiv.org/abs/2303.12570 (abstract details via search results only)
- HyperAgent: https://arxiv.org/abs/2409.16299 (abstract only)

Projects and documentation:
- SWE-agent ACI background: https://swe-agent.com/latest/background/aci/
- mini-swe-agent: https://github.com/SWE-agent/mini-swe-agent
- Aider repo map article: https://aider.chat/2023/10/22/repomap.html
- Aider repo map docs: https://aider.chat/docs/repomap.html
- Aider source: https://raw.githubusercontent.com/Aider-AI/aider/main/aider/repomap.py
- OpenHands runtime architecture: https://docs.openhands.dev/openhands/usage/architecture/runtime
- OpenHands condenser (SDK docs, seen in search results): https://docs.openhands.dev/sdk/guides/context-condenser
  and https://openhands.dev/blog/openhands-context-condensensation-for-more-efficient-ai-agents (snippet only)
- Claude Code subagents: https://code.claude.com/docs/en/sub-agents
- Claude Code memory: https://code.claude.com/docs/en/memory
- Claude Code best practices: https://code.claude.com/docs/en/best-practices
- Anthropic, writing tools for agents: https://www.anthropic.com/engineering/writing-tools-for-agents
- Codex CLI: https://github.com/openai/codex ; AGENTS.md: https://agents.md
- Cursor codebase indexing docs: https://cursor.com/docs/context/codebase-indexing
- Cursor blog, secure codebase indexing: https://cursor.com/blog/secure-codebase-indexing
- Windsurf Fast Context (docs, redirects to Devin docs): https://docs.windsurf.com/context-awareness/fast-context
  (only search-result snippets read; the redirect target was not fetched)
- Sourcegraph Cody context: https://sourcegraph.com/docs/cody/core-concepts/context
- Cline plan and act: https://docs.cline.bot/core-workflows/plan-and-act
- Continue context providers: https://docs.continue.dev/customize/deep-dives/custom-providers
- Moatless Tools: https://github.com/aorwall/moatless-tools

Not retrieved (all statements about these are REMEMBERED): Devin internals, Amp, CodeRAG, Roo Code details, Cline
tool names, Codex CLI tool set, Claude Code's choice of Grep/Glob over an index, Cursor rules format.

katana local documents consulted: `docs/code-intelligence.md` (sections 5 to 8), `docs/tools.md`,
`docs/code-index-lifecycle.md`, `CLAUDE.md`.
