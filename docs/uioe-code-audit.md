# UIOE code audit: what katana really has, against the three research lists

Status: audit of the code, read-only, 2026-11-23. Nothing was compiled, run or measured. Every claim below was checked by
reading `src/` (engine, agent, tools, coder, app), `desktop-qt/` and `tools/` in this worktree after `git merge main`.
Line numbers are from this tree and will drift; function names are the stable handle. Docs such as `docs/handoff.md`,
`docs/plan.md` and `docs/knowledge.md` were not used as evidence.

The three research documents audited:

- `docs/research/uioe-code-agents.md` section 15 (16 items, called A1..A16 here).
- `docs/research/uioe-context-memory-routing.md` section 19 (18 layers, called R1..R18).
- `docs/research/uioe-code-knowledge.md` section 16 (code-knowledge layer K1..K8, index lifecycle L1..L7, UIOE-level U1..U8).

Duplicates across lists are merged into one row with all source ids. Status words:

- **HAVE**: implemented and wired into the running agent (or CLI) as the item describes.
- **PARTIAL**: something real exists, but a named part is missing (stated in "Missing").
- **MISSING**: no code found (grep and reading; the search terms are given when the absence is surprising).

Gap size S/M/L follows the research documents' scale (S days, M weeks, L months) but is my own judgment of the work
left, given what already exists.

## 0. Shape of the code that matters for the UIOE

Facts the rest of the document relies on.

1. There is already a small pre-LLM stage, but it is not called that. In `src/agent/loop.c` (function `agent`, lines
   about 676-700), before the user's line enters the model, the loop runs in order: undo notice, `intent_set_user`
   (`src/tools/common.c:130`), `ds_select` (`src/agent/docsel.c:129`, rarity-weighted line selection from the `.md`
   files the project card points to, 600-token budget), `place_note` (`src/agent/locate.c:251`, finds a folder the user
   named but that does not exist from the cwd), `vis_take` (images), plan-mode note. Each appends text to the user turn.
   That is Interpretar/Recuperar/Context Builder for two narrow cases, with no shared structure.
2. Everything else is model-driven: the model calls tools one at a time (`loop.c` around lines 840-945) and the engine
   reacts after each call with `[katana: ...]` notes (about 10 sites in `loop.c`, 3 in `brief.c`, 3 in `read.c`, 5 in
   `delegate.c`, 1 each in `docsel.c`, `locate.c`, `context.c`, `code.c`, `findgraph.c`).
3. Tools are a table (`src/tools/table.c:6-51`, struct `{name, fn, readonly, desc, props, req}`), plus a legacy second
   set (`src/agent/dispatch.c`, `agent_tool_run`, names like `read_file`, `run_command`). The guards `brief_guard`
   (`table.c:190`) apply only to the table set. `ToolSearch` (`src/tools/toolsearch.c`) lets the prompt list some tools
   by name only (`src/agent/prompt.c:178`).
4. The code index has two separate stores: the Find index (`.katana/cache/find-<hash>.idx`, `src/tools/find.c`,
   `src/tools/findgraph.c`) and the coder segment store (`<folder>/store/`, `src/coder/store.c`, `storedocs.c`,
   `search.c`, `symindex.c`, `symquery.c`). `Find` calls `coder_lookup` (`search.c:1109`) for extra evidence.
5. `docs/investigation-engine.md` is a Python prototype (`tools/findbench/`, `tools/investigate/`); the CSSE/hypotheses
   machinery is not in C. grep for `hypothes` and `CSSE` in `src/` returns nothing.
6. One process = one model = one session (section 6 below).

---

## 1. Interpretation, classification, clarification

| ID | Item (sources) | Status | Evidence | What is missing | Gap | Depends on |
|---|---|---|---|---|---|---|
| I1 | Request interpreter: entities, constraints, intent, complexity (R1, A2) | PARTIAL | `ds_select` tokenizes the request with the model's own tokenizer and weights by idf (`docsel.c:129`); `place_note` extracts path-like fragments and "pasta/projeto/folder/project/repo <word>" (`locate.c:242-251`); `brief_example_note` detects examples in the request (`brief.c:301`, `br_has_example`); coder text pipeline `coder_tokens`, `coder_norm_word` (pt+en, `coder/text.c`); `intent_set_user` keeps the request for `Read` windows (`common.c:129-134`, `read.c:385` `rd_best_windows`) | No structure (entities, constraints, language, intent verb, complexity); each consumer re-parses the raw line with its own rules; no result object | M | `coder/text.c`, `coder/kb.c` |
| I2 | Domain and complexity classifier (R2, A2) | MISSING | Closest: `loop_plan_readonly` (`loop.c:414`) classifies a *tool* as read-only for the plan nudge, not the request. grep for classify/route/domain in `src/agent` finds nothing | Rule table request -> {code, files, web, memory, chat}; complexity -> plan gate or tool subset | S-M | I1 |
| I3 | Ambiguity and clarification policy (R3) | PARTIAL | `AskUserQuestion` (`src/tools/askuserquestion.c`) and `ask_user` (`src/agent/ask.c`: CHOICE marker to the desktop, numbered options in terminal, "decide alone" when stdin is a pipe); project `ask:` rules force a question before matching commands (`brief.c:208`, `brief_guard_subject:220`); the plan mode lets the model investigate and end with a plan (`planmode.c`) | The engine never detects ambiguity or decides "assume and say so"; asking is entirely the model's choice | S | I1 |
| I4 | Test and reproduction awareness; stack trace or failing test -> entry point; declared check commands (A9) | PARTIAL | Declared commands: the `test:`/`done:` keys of KATANA.md (`brief.c:314` `brief_end_note` warns once when files changed and the check did not run; `brief.c:301` examples note). Failure hints: `diagnose.c` (missing program, module, port busy), `bk_*`/`rp_*` loops (`loop.c:317-400`). Command ring: `cmdlog_add` (`exec.c:27`) | Nothing turns a traceback or a test name in the request or in an output into `file:line` candidates (grep for `Traceback`/stack frames in `find.c`, `loop.c`, `symquery.c` returns no parser); no suggested check command when `test:` is absent | M | I1, `Find`, `Symbols at` |

## 2. Sources, registry, routing, orchestration

| ID | Item (sources) | Status | Evidence | What is missing | Gap | Depends on |
|---|---|---|---|---|---|---|
| S1 | Source adapter interface / registry with capability declarations, cost, trust (A1, R4, K-L7, U2) | PARTIAL | Tool table `table.c:6-51` is a de facto registry with a `readonly` flag; MCP tools carry server annotations `ro/destr/idem/open` (`mcp.c:25`, trusted only when the server is the user's: `mcpx_readonly_trusted`, `mcp_extra.c:186-197`); LSP per extension (`tools/lsp.c`, env `KATANA_LSP_<EXT>`); `ToolSearch` deferral (`prompt.c:178`) | No per-source cost, freshness, trust class, failure vocabulary ("empty-certain" vs "empty-unknown" exists only inside Find/Symbols text: `find.c:1401-1436` NOT FOUND with the indexed-set limit) | M | none |
| S2 | Pre-LLM source router recording why each source was or was not consulted (A2, R5, U1) | MISSING | The only pre-LLM retrieval is `ds_select` and `place_note` (section 0); they do not log a decision (stderr line only: `loop.c:688`, `loop.c:694`). No code decides code vs web vs files before the first token | Router, route record, tool subset by route | L (general), S (code-only) | I1, I2 |
| S3 | Route planner: ordered route with dependencies and deadlines (R5) | MISSING | The model plans itself: `TodoWrite` (`src/tools/tasks.c`, `ct_todowrite`) and the older `todo` (`todo.c:27`). The engine only enforces having a plan (S6 below). Round budget: `rl_block` (`loop.c:524`, 40 rounds, +block on progress, cap 200) | Engine-built plan; deadlines per step | L | S2 |
| S4 | Orchestrator: parallel execution, retries, circuit breakers, rate limits (R6) | PARTIAL | Failure/loop control is real: `rp_*` refuses a call that failed identically twice (`loop.c:~878`, `rp_refuse`), `bk_*` "external obstacle" note on the 3rd and 6th failure around the same program/folder (`loop.c:332-400`, `bk_key_of`), `rr_*` stops repeated successful reads (`loop.c:470-497`), `rp_stuck` forces the closing round (`rl_forced`). Timeouts: `KATANA_CMD_TIMEOUT` 120 s (`terminal.c:4`), `MCP_CALL_MS` 60 s (`mcp.c:15`), curl `--max-time` (`web.c:55`). Web: `WebSearch` runs up to 4 queries and reads up to 5 pages in the same call (`websearch.c:1-4`, `web.c:303` meta-search) | No automatic retry with backoff, no rate limits, no per-source circuit breaker (breakers are per call signature, per request, in static arrays reset at each request: `loop.c:700`). Tool calls of one model turn run one after the other. No concurrency between sources | M | none |
| S5 | Bounded parallel retrieval subagent (A12) | PARTIAL | `Agent`/`delegate` (`src/tools/agent.c`, `src/agent/delegate.c:24`): same model, clean context, read-only (plan-mode lock), `SUB_ROUNDS 12`, `SUB_GEN 1200`; only the final report returns; `Workflow` runs several sub-agents in sequence (`tools/workflow.c`); `ListAgents`/`SendMessage` between katana sessions (`tools/messages.c`) | Sub-agents are serial: one model in memory, the main KV cache is saved to disk, the sub-agent runs from position 0, the cache is loaded back (header of `delegate.c`; `g_ctx_lost` forces recompute). The route set is chosen by the sub-agent's own model, not fixed | M | engine KV save/load (exists) |
| S6 | Mandatory plan and plan nudges (not in the lists; relevant to R5) | HAVE | `loop_plan_note` asks for `TodoWrite` on the 3rd and 6th non-read-only call without a plan and echoes the current step every 5 calls (`loop.c:424-440`); `loop_todo_nudge` once when the model ends with open steps (`loop.c:498-510`, uses `ct_task_sync`); list survives compaction (`context.c`, `facts.c`) | Plan content is free text from the model, never validated against the request | - | - |
| S7 | Cross-source evidence fusion with agreement counting (A3, R8, U5) | PARTIAL | Inside code: RRF over six channels exact/subword/bm25/trigram/literal/conceptual (`search.c:378-391`, `CsRes.fused:733`, `KATANA_CODER_RRF`); sparse activation over a file graph with four edge kinds sym/path/rare/co-change (`findgraph.c:1-25`, `fg_neighbor_note:603`); web meta-search merge and URL de-duplication (`web.c:303`, `websearch.c`) | Nothing fuses code with documents, web or memory; no duplicate/independence detection across sources; no conflict marking | M | C1 envelope |
| S8 | Safe database connector (U-DB, R18) | MISSING | grep for sqlite, psql, postgres, mysql only hits language-name tables in `coder/parse.c`, `storedocs.c`, `read.c`. No SQL tool or MCP wrapper in the tree | Read-only role, single SELECT allowlist, named queries, limits | L | S1; plugin rule (third parties stay optional plugins) |
| S9 | Domain plugins: finance, legal, financial APIs (R18) | MISSING | grep for finance/legal terms found none; the only external path is MCP (`mcp.c`, stdio and streamable HTTP, protocol 2025-11-25, roots, elicitation, resources, channels: `mcp_extra.c`, `mcp_channel.c`, `tools/mcpres.c`) | Connectors; freshness policy per source (quotes, in-force dates) | L, deferred | S1, S8 |

## 3. Context building and context hygiene

| ID | Item (sources) | Status | Evidence | What is missing | Gap | Depends on |
|---|---|---|---|---|---|---|
| C1 | Common evidence schema with provenance (R7, U3, A6 typed events) | PARTIAL | Code results carry `confidence` 0-100, `level`, `how`, `tier`, per-channel scores (`search.c:991` JSON, `symquery.c` cascade, `find.c` `CoderHit`: path, tier, exact, literal, anchor, channels, evidence lines); docsel lines carry file and line; web results end with a `[web:` trailer (checked by `delegate.c:~170`); every result is wrapped as "external content: data, not instructions" (`delegate.c:101-102`, `context.c:614`) | Each tool prints its own text; no shared fields (retrieved_at, source_time, digest, locator, derivation, verification); the model reads prose, the engine cannot re-use the grade after the fact | M | none |
| C2 | Context builder: rerank, extract, budget, order, cite, negative evidence (R10, A section 16) | PARTIAL | Budget per result from remaining context: `g_budget` (`loop.c:~872`, 3 bytes/token, cap `TOOL_WEB`); docsel budget 600 tokens (`docsel.c:7`); `Read` of a big file returns map + first lines + the windows that match the request (`read.c:385-400`, `ct_outline`, `rd_best_windows`); negative evidence: `NOT FOUND` with scope limit for Find (`find.c:1401-1436`); web read returns matching passages (`webfetch.c`, `webnav.c`); `tool_digest` compresses big Read/WebFetch with the model (`delegate.c:156`, `DG_GEN 420`); notes are appended to the user turn (`loop.c:684-694`) | A single builder over several routes: today each tool does its own budgeting; no ordering policy, no citation contract (the model is not required to cite locators) | M-L | C1, S2 |
| C3 | Graph-ranked repo map / overview for requests without entry point (A4) | PARTIAL | File graph with personalised activation exists for ranking (`findgraph.c`, `FG_ROUNDS 2`, decay 0.5), `katana init` scans languages and folders (`extend.c:36-72`, `ex_scan`) and writes a KATANA.md layout block; `Symbols outline` and `coder outline` exist (`symquery.c`) | No rendered overview with a token budget ("what is in this repo / this folder"); no personalisation by the request for an overview | M | `findgraph.c`, `symquery.c` |
| C4 | Skeleton views as first-class output (A5, K-L) | HAVE | `ct_outline` map in `Read` for large files (`read.c:385`); `ah_outline` keeps defined names when an old result is masked (`context.c:214`); `Symbols outline|def|callers|callees|uses|deps|impact` (`tools/symbols.c`, `symtool.c`) | Not used for non-target files in a pack (no pack) | S | - |
| C5 | Seen-span register / investigation state (A6, R11) | PARTIAL | `ct_seen` marks files read whole or in part and notices a change by size+mtime (`common.c:140-208`); `rc_note` records how many leading lines were already delivered and `read.c:380` continues from there; `rd_delta` returns only what changed when a file is re-read while the first reading is still in context (`facts.c:99-114`, `KATANA_READ_DELTA`); `facts.c` notebook: files changed/read, each command with last exit code, last unfixed failure (`facts.c:35-90`), re-injected on compaction; `fg_neighbor_note` lists neighbours not yet read (`findgraph.c:603`) | Coverage is prefix-only per file (`cov_end`), not arbitrary spans; no hypotheses, no "ruled out" list; reset per request or conversation (`ct_seen_reset`, `rr_reset`), the session file keeps messages but not this state | M | I1 |
| C6 | Observation masking / collapsing of old results (A7) | HAVE | `ah_mask` (`context.c:232`): drops results oldest first, keeps the last `AH_KEEP_ROUNDS`, keeps the newest whole read of each file, replaces others by a "removed to save context" stub with an outline of defined names (`ah_outline`), the tail of a Bash output, and a disk stash to re-read (`cp_stash:179`); superseded reads and their "changed/unchanged" notes leave together; images weighed by `vis_weight` | Policy is size/age based, not relevance based | S | - |
| C7 | Context compaction in layers (not in the lists) | HAVE | `agent_compact` (`context.c:469`): (1) old requests' tool bodies shrunk, (1b) masking in the current request without the model, (2) model summary only if still needed (`ah_summary:326`, live variant `ah_summary_live:407`), (3) shrink current results; debt model `cp_due:97` with `KATANA_CONTEXT_BASE`, hard limit 60% of context (`cp_hard:92`); facts notebook injected; `KATANA_COMPACT=0` off. Session save has its own cheaper compaction (`sess_compact`, `loop.c:144`) | The summary uses the model (research assumed deterministic only) | - | - |
| C8 | Prefix cache and fast resume (not in the lists) | HAVE | `prefix_cache.c`: KV (and DeltaNet state) of the stable system prefix saved to `.katana/cache/pfx-<key>.kv`, key = model, tokens, context, backend (`pfx_key:77`, `pfx_save:154`, `pfx_load:210`, `pfx_warm:325`); session resume reloads KV or recomputes, with expert warm-up (`loop.c:250-309`, "heat") | - | - | - |
| C9 | Rule-triggered context injection (A11) | PARTIAL | KATANA.md layers: global, project (`KATANA.md`/`AGENTS.md` up to repo root), local, `@include` two levels (`project.c:1-30`); a KATANA.md in a subfolder arrives once with the first Read/Edit/Write of a file in it (`proj_sub_note`, `project.c:148`); skills listed in the environment line (`sk_env_line`, `tools/skill.c`); docs graph via the `docs:` block (docsel) | No path-glob or keyword triggered rules; `rules:` are always on; no rule that fires on a term in the request | S-M | I1 |
| C10 | Explicit sufficiency decision (A13, R9) | MISSING | Proxies only, all reactive: `rr_check` tells the model to "answer with what you have" after repeated reads (`loop.c:475`); round limits and the closing note at `round == lim - 2` (`loop.c:~940`); `brief_end_note` (verification not run). No recorded "enough, because" and no second round driven by it | Sufficiency check (named entities found, literal anchors found, negative evidence listed) before the model's first answer round | S-M | C1, I1 |
| C11 | Draft-as-query second round (A14) | MISSING | One adjacent mechanism: after `Find`, `g_intent_find` replaces the request as the window query of the next `Read` (`find.c:1522`, `common.c:134`). Nothing feeds a first hypothesis back into retrieval | Second retrieval round with terms from the first answer | S | route layer |
| C12 | Response-format switch concise/detailed (A15) | PARTIAL | Output size is controlled by parameters: `Read` offset/limit, `WebSearch` `read=0..5`, `Find` `limit` (default 6, max 12), `Symbols --lines`, `Grep output_mode` | No uniform `response_format` knob | S | - |
| C13 | User-visible "what was included and why" (A16) | PARTIAL | stderr/UI lines: `[docs: N trechos...]` (`loop.c:688`), `[katana achou a pasta...]` (`loop.c:694`); UI markers TOOL/TOOLOUT/TODO/TOKENS/COMPACT (`ui_tool_mark`, `ui_toolout` in `dispatch.c`); desktop QML `ToolCard.qml`, `ActivityGroup.qml`, `ContextMeter.qml`; `agente.log` | The docsel lines themselves are not shown (only their count); no pack view because there is no pack | S | C1 |
| C14 | Optional model as expander / tie-breaker (R17) | PARTIAL | The model is already a helper outside the main loop: `ah_summary`/`ah_summary_live` (greedy, no tools), `tool_digest_in` (note for a big Read/WebFetch, 64 cached slots `g_dg`, `delegate.c:150-250`) | No query expansion by the model, no label tie-break under a grammar. (Engine support exists: `callgrammar.c` constrains tool calls to valid names/keys/types, `KATANA_CALL_GRAMMAR`) | S | callgrammar |
| C15 | Hash-tree freshness / per-line freshness (A10, K-L4) | MISSING (digests PARTIAL) | Each catalog entry has size, mtime (ns) and a 64-bit content hash (`storedocs.c:4,17`); incremental update compares (size, mtime) and, if mtime changed, hash (`storedocs.c:345`). `Find` re-reads files whose date or size changed on every call (`find.c` header) | No tree of hashes (no "is this subtree unchanged" in O(1)); no racy-timestamp rule (equal size and mtime with different content is not caught) | M | - |

## 4. Memory, sessions, learning, traces

| ID | Item (sources) | Status | Evidence | What is missing | Gap | Depends on |
|---|---|---|---|---|---|---|
| M1 | Fact memory with validity, provenance, trust (R12) | PARTIAL | `remember` / `recall` (`tools.c:493`, `tools.c:565`): one line `date<TAB>fact` in `~/.katana/memoria.txt` (`KATANA_MEM`), exact-duplicate check, recall = word-hit count over at most 4096 lines, ties to the newest, 8 results; the last 20 facts are placed in the system prompt every time (`prompt.c:144`, `mem_tail`); the memory tools and the injected facts are off in `katana code` mode (`g_code`, `prompt.c:144,234`) | No subject/predicate/value, no interval, no source or trust, no update or forget tool (grep `forget` finds none), no relevance gate on the 20 injected facts, `mem_read` reads only the last 1 MB | M | I1 |
| M2 | Cross-session retrieval of the agent's own history (not in lists) | PARTIAL | Sessions `.ksess`: messages, KV and expert heat (`sess_save:215`, `sess_resume:250`, block `KSX1` for training `loop.c:182`); `/resume`, `--retomar`; `katana knowledge build` makes Markdown notes (request, last model line, files written, commands, errors overcome) from *transcripts of another coding agent* (`app/knowledge.c`, `app/transcript.c`) so the agent can Grep them | No notes are built from katana's own sessions for later retrieval; resume is whole-session only | M | `knowledge.c` |
| M3 | Undo of the agent's file changes (not in lists) | HAVE | `undo.c`: previous version of a file saved before Write/Edit in `~/.katana/undo`, `UNDO_N 400`, `/undo` reverts the last request that changed something and tells the model (`loop.c:648-651, 676-680`) | Shell side effects not covered; files over 8 MB not saved | S | - |
| M4 | Decision trace and eval hooks as JSONL (R15, U8) | PARTIAL | `agent_log` writes one tab-separated line per tool call to `agente.log` (`tools.c:148`) but cuts arguments to 400 and the result to 200 characters (`a[400]`, `r[200]`), `mcp_log` for MCP (`mcp.c:567`); `katana agentlog` summarises failures, repeats and longest failure runs (`agentlog.c`); sessions carry the exact rendered text (KSX1); eval scripts `tools/investigate/` (traces/ dir), `tools/codebench`, `app/codebench.c` | No structured decision trace for pre-LLM steps (docsel, place_note, notes, nudges are stderr lines, not records); log is lossy and not replayable; no redaction of secrets in arguments | S | - |
| M5 | Learning loop from sessions (R16) | PARTIAL | `katana export` sessions -> JSONL with train spans (`app/learn.c:2,272`), `katana learn` rounds with eval (`learn.c:471`), `import`, `distill`, `train`, `specialize`, curriculum generators (`app/curriculum.c`, `scenario*.c`) | All of it trains the model; nothing learns routing, tool subsets or a classifier from logs | M | M4 |

## 5. Verification, safety, extension points

| ID | Item (sources) | Status | Evidence | What is missing | Gap | Depends on |
|---|---|---|---|---|---|---|
| V1 | Edit-time syntax verification, with revert (A8, R13) | PARTIAL | `code_check` (`code.c:139`) runs after Write (`write.c:53`), Edit (`edit.c:174`), edit_file (`devtools.c:224`), write_file (`dispatch.c:88`): python `ast.parse`, `bash -n`, json load, `node --check`, `php -l`, `ruby -c`, 10 s timeout, appends `[syntax check: ok]` or `FAILED ... fix this now`. Edit also needs a whole prior Read (`ct_seen`), unique match, atomic multi-edit (`edit.c:1-8`) | No check for C/C++/TS/Go/Rust/Java (the coder parsers cover many languages but have no error or "recovered" state wired to writes); no automatic revert (the file stays broken; `undo.c` is user-driven); `multiedit.c`/`notebookedit.c` paths vary; check is skipped on Windows | S-M | `coder/parse.c` |
| V2 | Verification layer: quote and locator checks, citation checks (R13) | PARTIAL | `Find` literal check: a quoted string or name is checked exactly and answered NOT FOUND (`find.c:1327-1436`); `Edit` refuses stale or non-unique text; LSP `diagnostics` operation (`tools/lsp.c`, manual); project `test:`/`done:` reminders (`brief.c:314`) | Answers are not checked against the evidence (no citation check, no quote check); nothing runs the project check automatically | M | C1 |
| V3 | Security layer: trust classes, taint, egress, redaction (R14, U7) | PARTIAL | Data-not-instructions wrapper and `defang` of `<|`, `<tool_call>`, `<tool_response>` in results (`dispatch.c:191-205`, prompt rule `prompt.c:171`); `agent_act` (`tools.c:100`): no prompt by owner decision, `KATANA_CONFIRM=1` / `KATANA_TRUST=0` ask each time (`trust_init:306`); reads outside the folder need a grant (`agent_read_access`, `tools.c:239`); `--allowed-tools/--disallowed-tools` (`launch.c:371`); plan mode refuses every write in the engine (`planmode.c`); project `never:`/`ask:` rules enforced in `ct_dispatch` (`table.c:190`, `brief.c:208-258`); project MCP servers must be trusted by path+hash (`mcp.c:795-837`); `KATANA_WEB=0` switch (`web.c`, `dispatch.c`); `serve --agent` refuses non-loopback without `KATANA_API_KEY` (`server.c:910`); sudo password never over the network (`agentapi.c`); other-session messages marked as data (`table.c:259`) | No taint: content from web/MCP/files can reach `Bash`/`Write` without a gate (the owner postponed this on purpose, plan backlog T2); no egress allowlist; no secret redaction in `agente.log`; guards do not cover the legacy tool set (`dispatch.c`) for never/ask | M | owner decision (no-confirm rule) |
| V4 | Hooks: user commands before/after a tool call (not in lists) | MISSING | grep `hook` in `src/agent`, `src/tools`: only `cmd_term_hook` (terminal cleanup, `exec.c:118`) and a test hook in `messages.c:264`. `never:`/`ask:` patterns act as a restricted pre-tool policy, nothing runs a user program | Pre/post tool hooks, event hooks (session start, stop) | S-M | table.c dispatch point |
| V5 | Skills and extension packs (not in lists) | HAVE | `Skill` tool (`tools/skill.c`): `SKILL.md` folders in project and home `.katana/skills`, `.config/katana/skills`, built-in recipes (`howto.c`); `katana skill new/check/install` and `/skill` (`extend.c:238-319`, `592`); ability and card formats are covered by `app/curriculum.c` training data | No path-scoped skill activation | - | - |
| V6 | MCP client and server (not in lists) | HAVE | Client: stdio and streamable HTTP, `servidor__ferramenta` naming, timeouts, trust, roots, elicitation, resources, channels, `readOnlyHint` plan gating (`mcp.c`, `mcp_extra.c`, `mcp_channel.c`, `tools/mcpres.c`). Server: `katana mcp serve` exposes Read, Glob, Grep, Write, Edit, NotebookEdit and (with `--allow-shell`) Bash, root-confined, `never`/`ask` honoured (`mcp_serve.c:12-23`) | The server does not expose `Find`, `Symbols` or `LSP` (list `g_mcps_tools`), so external clients cannot use the code index | S | - |
| V7 | Terminal and process control (not in lists) | HAVE | `Bash` (`tools/bash.c`), pty sessions `terminal`/`terminal_screen` (`terminal.c:52`, `posix_openpt:160`), background jobs `job` (`jobs.c`), `Monitor`/`TaskStop`, root shell, process-group reaper that outlives a SIGKILL of the agent (`reaper.c`), sudo detection (`sudo.c:pw_detect`), `read_log` (`logs.c`) | - | - | - |
| V8 | Tests of the tools and engine (not in lists) | HAVE | `katana tool <name> <json>` drives any tool without a model (used by `tools/testar_ferramentas.sh` with an isolated HOME and `KATANA_LOG`, `KATANA_MEM`); shell suites `tools/tooltest/*.sh` (agents, bash, channel, extend, files, find, launch, loopnotes, lsp, mcpx, pfx, reap, regex, schedule, search, tasks, web with mocks `mcpfake.c`, `ssefake.c`, `websrv.py`); `katana selftest <model> [--deep]` (`main.c:1233`, `deeptest.c`); `kerneltest`, `gradcheck`, `attbench`; `codebench` judge with hidden tests (`app/codebench.c`); scenario/curriculum engines that run real tool calls and keep only verified conversations (`scenario.c`) | Not run in this audit | - | - |

## 6. Code-knowledge layer (K), index lifecycle (L), federation (U)

| ID | Item (sources) | Status | Evidence | What is missing | Gap | Depends on |
|---|---|---|---|---|---|---|
| K1 | Stable human-readable symbol ids with global/local split and package (K1) | MISSING | Symbols are rows of `SY_T_SYMS` keyed by name id (position in a per-segment sorted string table) and document-local indexes (`symindex.c:5-20`); queries match by name | Global ids, package/module component | M | symindex format version |
| K2 | Per-occurrence role bits and enclosing symbol (K2) | PARTIAL | `SY_T_REFS` row: name, kind, **origin symbol** (the enclosing one), line, col, flags (`symindex.c:10`, `SyRIn:45`); `Find mode=refs` prints "the function around each use" | Role bits (definition/read/write/import) are only whatever `kind`/`flags` encode; not a stable, documented role set | S | - |
| K3 | Candidate-set resolution with precedence and reasons; import/scope resolution without a build (K3) | PARTIAL | Cascade: exact name (92) -> derived (62) -> edit distance <= 2 (32), ambiguity penalty 5 per extra definition up to 25, `how` string per result (`symquery.c:1-25`); `deps` and `impact` commands; `flow.c` links by identical text key with a confidence recipe (route method match +10/-25, env +14, same file -20...) | No scope or import resolution (links are by name), so a call to `init` cannot be told apart by what the file imports; no precedence rules by language | L | parsers |
| K4 | Parse-quality flag per file (clean, recovered, text-only) (K4) | MISSING | Per symbol `conf` is `CC_EXACT`/`CC_HEUR` (`parse.c:384`, `1847` macros) and `vis`; no per-file state; no "partially parsed" signal reaches `scope_limits` or the model | File flag, surfaced in results | S-M | `coder/parse*.c` |
| K5 | Re-anchoring of stale positions (K5) | PARTIAL | Index refresh on demand: `sym_ensure_index` updates the coder index before every `Symbols`/`Find refs` call (`symtool.c`), Find re-reads changed files each call; `Edit` accepts a file changed on disk if the old text still matches (`edit.c:2`) | No stored-line-text/pattern re-anchoring for answers already given to the model (an old `file:line` is not corrected when the file changes within the session) | S | `ct_seen` |
| K6 | Typed uncertainty on edges (speculative / pattern / name-only) (K6) | PARTIAL | Every query result has `confidence`, `level` high/medium/low and `how`; `CC_HEUR` for macro-like symbols; flow links have base 60 and listed adjustments (`flow.c:9-20`) | Edge kinds are not separate types in the index; confidence is computed at query time | S | - |
| K7 | Contract-aware linking: OpenAPI, AsyncAPI, .proto as evidence of a link (K7) | PARTIAL | Contract files are a source of keys (role DEFINE, class `contract`, `keys.c` block "CONTRATOS"): OpenAPI/Swagger, AsyncAPI, `.proto`, GraphQL, JSON Schema and typed enums/unions define the key; `flow` shows the contract as the third side and `contract -> code` links with a confidence per rule (`fl_pair_contract`, `flow.c`); see `docs/code-intelligence.md` 12.8 | Built and checked by hand on small test projects and on the JSON Schemas of one real project; no OpenAPI, proto or GraphQL file from a real project was available to check. Not covered: schema-level OpenAPI `$ref` resolution, Thrift, typed enums outside TypeScript/Python/Go | S | `keys.c`, `flow.c` |
| K8 | Signature/body split so body edits do not invalidate links (K8) | PARTIAL | Symbol rows store `sig_off/sig_len` and `body0` separately (`symindex.c:8-9`); update is per changed file (segments immutable, `storedocs.c:345`) | Links of unchanged files are not kept when a callee's body changes, because refs are name-based and re-resolved at query time (so this is mostly moot here) | S | - |
| L1 | Dynamic overlay over the background index with snapshot reads (L1) | MISSING | Manifest versions give readers a stable snapshot (`store.c` header: segments immutable, MANIFEST replaced by rename; readers need no lock). No per-file overlay for unsaved or just-written files | Overlay layer | M | - |
| L2 | Content-digest keys including analyzer version; before/after stability check (L2) | PARTIAL | Content hash 64-bit per document (`storedocs.c:17`); section versions in the store format (`store.c` section table: type, version, checksum) | Analyzer version in the key; stability check around analysis | S | - |
| L3 | Atomic publish, checksums, quarantine, fallback tier, expiry tied to readers (L3) | PARTIAL | Atomic: write tmp, fsync, rename of MANIFEST (`store.c:1-25`); checksums of sections, table and manifest (`store_seg_verify`, `coder verify`), `coder compact` merges segments, single-writer file lock | No quarantine or fallback tier on corruption (verify only reports); no reader-tied expiry of old segments | S | - |
| L4 | Watcher overflow handling, racy-timestamp rule (L4) | MISSING | No inotify or watcher anywhere (grep inotify/fsnotify: none). Freshness is pull-based: `autoindex_dir` spawns `katana coder index <dir>` in the background when the agent starts or on `/cd`, and for a folder the user names (`loop.c:453-466, 573, 594, 692`); `Find`/`Symbols` update before answering | Event-driven updates; racy rule | M | - |
| L5 | Coverage report and skip-reason codes feeding `scope_limits` (L5) | PARTIAL | `katana coder status` shows segments, files, size, version (`storedocs.c:480`); "NOT FOUND ... limit of the indexed set" text (`symquery.c`, `find.c:1436`); directories skipped by a fixed list (`parse.c:1958` `SKIP`, plus `.gitignore` in Find) | Per-file skip reasons (too big, binary, parse failed) in the answer | S | K4 |
| L6 | Cancellation with a mandatory response and partial-result markers (L6) | PARTIAL | Ctrl+C: `g_stop` ends the generation and no tool runs (`loop.c:815`); LSP and MCP calls have deadlines (`lsp.c` poll deadlines, `MCP_CALL_MS`); `Read` marks `[PARTIAL view` | No cancellation inside `Find`/`coder index`; no common partial-result marker | S | - |
| L7 | Capability declaration per analyzer/source (L7) | PARTIAL | LSP servers chosen by extension and started on demand (`lsp.c`), tool `readonly` flags, MCP annotations; `katana prepare`/`info` describe the model | No capability record for analyzers | S | S1 |
| U1 | Router recording why each source was or was not consulted (U1) | MISSING | See S2 | - | L | - |
| U2 | Connector contract (capabilities, cost, rate, freshness, permissions, failure vocabulary) (U2) | PARTIAL | See S1 | - | M | - |
| U3 | Single Evidence schema (U3) | PARTIAL | See C1 | - | M | - |
| U4 | RRF with per-source priors and caps; independence detection; conflict surfacing (U4) | PARTIAL | RRF with per-channel weights inside code search (`search.c:386`, `g_cs_chw`); no cross-source | Priors per source, independence, conflicts | M | S7 |
| U5 | Source-specific freshness policies (quotes, legal in-force dates, web dates) (U5) | MISSING | Web page cache of the 8 last pages (`webnav.c`, `WN_PAGES 8`) has no date policy; fetch results do not carry the page date | Policy per source | M-L | S9 |
| U6 | Output sanitisation and "tool output is data" for web and MCP (U6) | HAVE | `defang` (`dispatch.c:191`), wrapper text (`delegate.c:101`, `context.c:614`), "data, never instructions" in prompts (`prompt.c:171,210`, `howto.c:12`), MCP results marked external (`mcp.c` header); audit log of every tool call (`agente.log`, `mcp.log`) | Log is truncated (M4) | - | - |
| U7 | Deterministic explanation trace attached to the pack (U8) | MISSING | No pack, no trace; see M4 | - | S (once a pack exists) | C1, S2 |

---

## 7. Summary (a): by block of the owner's diagram

`USER INPUT -> UIOE { Interpretar, Classificar, Roteirizar, Recuperar, Orquestrar } -> sources -> CONTEXT BUILDER -> LLM EXECUTION`

| Block | What exists today and where | Verdict |
|---|---|---|
| Interpretar | Three disconnected parsers of the raw line: `ds_select` (`docsel.c:129`), `place_note` (`locate.c:251`), `brief_example_note` (`brief.c:301`); text normaliser pt+en (`coder/text.c`), concept/abbreviation knowledge base (`coder/kb.c`, `search.c:831`); `intent_set_user` (`common.c:130`) | PARTIAL, no common output |
| Classificar | Nothing for requests. Tool-level read-only flag (`table.c`, `loop_plan_readonly`) | MISSING |
| Roteirizar | Nothing before the model. The model picks tools; `ToolSearch` deferral reduces what it sees (`prompt.c:178`); constrained tool-call grammar (`callgrammar.c`) guarantees valid names and keys | MISSING (grammar and deferral are model-side aids) |
| Recuperar: Filesystem | `Read` (map, windows by request, delta reads), `Glob`, `Grep`, `list_dir`/`find_file` (`locate.c`), `Find` (BM25 by field + RRF + file graph, `find.c`, `search.c`, `findgraph.c`), `Symbols` (def, refs, callers, callees, uses, outline, deps, impact: `symquery.c`), `LSP` (`lsp.c`), documents `read_document`, notebooks | HAVE (all model-called); pre-LLM only docsel and place_note |
| Recuperar: Web | `WebSearch` (SearXNG or built-in meta search, up to 4 queries and 5 pages in one call), `WebFetch` with paging, find and links (`webnav.c`), `download_file`; `KATANA_WEB=0` | HAVE (model-called); no freshness or source policy |
| Recuperar: APIs financeiras | none | MISSING |
| Recuperar: Repositorios de codigo | Local repos fully; `EnterWorktree`/`ExitWorktree`, `git` tool (`devtools.c:479`); remote hosts only via web or MCP | PARTIAL |
| Recuperar: Fontes juridicas | none | MISSING |
| Recuperar: Bancos de dados | none (only MCP could carry it) | MISSING |
| Recuperar: Memoria e sessoes | `remember`/`recall` (`tools.c`), last 20 facts in the system prompt, `.ksess` resume, `knowledge` notes of foreign transcripts | PARTIAL |
| Orquestrar | Sequential tool execution; failure and loop control (`rp_*`, `bk_*`, `rr_*`, `rp_stuck`); timeouts; sub-agent with clean context (`delegate.c`) and `Workflow` (sequential); background `job`, `Monitor`, `CronCreate`/`ScheduleWakeup` (`schedule.c`); MCP channels push work in (`mcp_channel.c`) | PARTIAL (no parallel sources, no retries) |
| Context Builder | Per-tool budgets (`g_budget`), `Read` map and windows, docsel budget, NOT FOUND negative evidence, masking and compaction (`context.c`), facts notebook (`facts.c`), digest of big results (`delegate.c:156`), prefix cache (`prefix_cache.c`) | PARTIAL: strong hygiene, no builder |
| LLM Execution | Engine (`src/engine`: Vulkan and CPU, Qwen3.6 MoE path), chat formats ChatML/Gemma 4/harmony/GLM (`fmt_*.c`), constrained tool-call grammar, KV save/load, speculative paths per model, streaming UI markers, `serve` OpenAI-compatible API (`server.c`), agent over the network (`agentapi.c`), the loop (`loop.c`) | HAVE |

Counts over the 63 table rows (section 1 to 6): HAVE 11 (S6, C4, C6, C7, C8, M3, V5, V6, V7, V8, U6), MISSING 15
(I2, S2, S3, S8, S9, C10, C11, V4, K1, K4, L1, L4, U1, U5, U7), PARTIAL 37 (C15 counted as partial). Rows U1-U7 repeat
earlier rows, so the distinct count is lower.

## 8. The ten largest real gaps, by value for fewer model steps

Ordered by how many model rounds each would remove on the requests katana serves (code questions and edits in a folder,
plus file and web chores). The ordering is judgment from reading the loop; nothing was measured (owner rule).

1. **A pre-LLM retrieval pack for code requests.** Today the first 2-8 model rounds are `Find`, `Symbols`, `Read`
   chosen by the model. The pieces exist (autoindex at start, `coder_lookup`, `Find` ranking, `Read` windows); what is
   missing is calling them from the engine before the first token and appending the best locators to the user turn, the
   way `ds_select` already does for docs (`loop.c:684`). Needs I1 and a budget; M.
2. **Request interpreter plus classifier (rules).** One struct (entities, identifiers, paths, quoted strings, language,
   verb, domain, complexity) feeding gap 1, the plan gate, tool subsetting for the prompt, and the sufficiency check.
   Reuses `coder/text.c` and `kb.c`. M.
3. **Stack trace / failing test to entry point.** No parser exists (I4). A traceback or test id in the request or in a
   Bash result should become `file:line` calls to `Symbols at`/`Read` without a model turn. M.
4. **Overview for requests without an entry point** ("what does this repo do", "where do I add X"): ranked repo map with
   token budget over `findgraph.c` and `symquery.c outline` (C3). The model currently spends many rounds listing and
   reading. M.
5. **Sufficiency / stop logic and second round** (C10, C11): decide from evidence grades (all named entities found,
   literals confirmed, NOT FOUND recorded) whether the model should answer now, and feed the first hypothesis back as
   query terms. S-M.
6. **Persistent investigation state** (C5): spans (not only a prefix) already shown, ruled-out files, hypotheses;
   kept in `facts.c` form, survives compaction (it already does for facts) and session resume (it does not). M.
7. **One evidence envelope** (C1): source, locator, grade, freshness, digest on every result, so masking, the builder,
   fusion and the trace can reuse them. Today only the coder JSON has grades. M.
8. **Write-time verification beyond six languages, with automatic revert** (V1): extend `code_check` to C/C++, TS, Go,
   Rust, Java (compiler `-fsyntax-only`, `tsc`, `go vet`, `cargo check` when present, or the coder parser with an error
   state) and restore the previous content (the `undo_note` copy already exists) when it fails, telling the model.
   Removes the "fix it now" round trips. S-M.
9. **Retrieval over the agent's own history and a real fact memory** (M1, M2): relevance-gated facts instead of the last
   20, `forget`/update, a validity date, and notes built from katana sessions (not only foreign transcripts). M.
10. **Source registry with decision trace and hooks** (S1, S2, M4, V4): a table of sources with cost/trust/freshness
    that the router fills, a lossless JSONL record of each pre-LLM decision, and pre/post tool hooks. Prerequisite for
    non-code domains (finance, legal, databases, which are L and deferred as plugins). M.

Not in the top ten but cheap and real: expose `Find`/`Symbols`/`LSP` in `katana mcp serve` (V6, S); response-format knob
(C12, S); parse-quality flag (K4, S-M).

## 9. (c) Where the research was wrong about katana, and what it did not list

### Research said missing or unknown, the code has it (or has most of it)

| Claim in the research | What the code shows |
|---|---|
| A7 "observation masking not described in the docs read" | HAVE: `ah_mask` with stash and outline (`context.c:232`), plus layered compaction (`agent_compact:469`) and a debt model (`cp_due:97`) |
| A8 "mandatory check-and-revert may or may not exist" | The check exists (`code.c:139`) for six languages; the revert does not (V1) |
| A6 "no register of spans already shown" | PARTIAL: `ct_seen`, `rc_note`/`cov_end`, `rd_delta`, `facts.c`, `rr_*` (C5) |
| A11 "rule-triggered injection" absent | PARTIAL: subfolder KATANA.md on first touch (`project.c:148`), skills line, docs graph |
| A12 bounded retrieval subagent absent | PARTIAL: `Agent`/`delegate` with 12 rounds, read-only (S5) |
| A16 "what was included and why" absent | PARTIAL: stderr and UI markers, desktop tool cards (C13) |
| A4/A5 skeleton and repo map | Skeletons HAVE (`ct_outline`, `ah_outline`, `Symbols outline`); repo map really missing |
| R3 ambiguity policy "None" | PARTIAL: `AskUserQuestion`, `ask_user`, `ask:` rules |
| R6 "failure detection only" | Understated: refusal of repeated failing calls, forced closing round, external-obstacle notes, timeouts (S4); true that there are no retries or rate limits |
| R11 "session history; facts as text" | There is a typed notebook (`facts.c`) with files, commands, exit codes, last failure, and a mandatory plan list (`todo.c`, `tasks.c`) |
| R13 "execution checks by the model" | Engine-side checks: `code_check`, `brief_end_note`, `brief_example_note`, Find literal NOT FOUND |
| R14 "action log; opt-in confirmation" | Much more: defang, trust per folder, never/ask rules, plan mode lock, allowed-tools, MCP path+hash trust, API key rule (V3) |
| R17 "model only as the agent" | The model is also used for summaries and digests (C14) |
| R10 "docsel plus compaction" | Also read-by-difference, windows by request, negative evidence, prefix cache, digest |
| K-L3 atomic publish, checksums, compaction (listed as absent) | HAVE for publish and checksums and compaction (`store.c`, `coder verify|compact`); quarantine and reader expiry missing |
| K-L1/L2 freshness | Update-on-demand before each `Symbols`/`Find` call and per-document content hash exist (K5, C15) |
| "Summarisation: deterministic only; no model summaries" (design note, code-agents section 16) | katana already summarises with the model when masking is not enough (C7); the deterministic layers run first |

### Things that are real and the research did not list

1. `Read` by difference and windowed reads (`facts.c:99-114`, `read.c:380-400`): large and repeated reads are the biggest
   context cost; this is a retrieval-shaped feature living inside a tool.
2. Constrained tool-call decoding (`callgrammar.c`): names, keys and types valid by construction (lazy mask). It lowers
   failed calls without any retrieval layer.
3. Tool deferral with `ToolSearch` (`prompt.c:178`), the nearest thing to tool routing (RAG-MCP in the research).
4. Prefix and session KV persistence, expert warm-up on resume (`prefix_cache.c`, `loop.c:250-309`).
5. Process hygiene: reaper that kills orphans after SIGKILL (`reaper.c`), pty terminals, background jobs.
6. The training loop (`learn`, `export`, `import`, `distill`, `curriculum`) which produces conventions as data.
7. `katana mcp serve`: katana as a tool server for other agents.

### Weak points the code shows and the research did not name

1. **Two code indices with different tokenizers and rankers** (Find `.idx` and the coder store). `Find` is the tool the
   model uses and carries the graph; `Symbols` uses the coder store. A UIOE route layer would have to query both or
   merge them.
2. **Notes are advice, not control.** Every `[katana: ...]` note can be ignored by the model; only a few paths enforce
   (plan-mode lock, `rp_refuse`, `rl_forced` closing round, never/ask guards).
3. **The decision data is lost.** `agente.log` keeps 400 characters of arguments and 200 of result, and pre-LLM
   decisions go to stderr only (M4); that blocks any learning of routing and any audit of the UIOE.
4. **Two tool sets.** The legacy set in `dispatch.c` and the table set in `tools/` have different guards, different
   notes and different edit semantics; `brief_guard` runs only for the table (`table.c:190`).
5. **Windows divergence.** Large parts are `#ifndef _WIN32` (Find, Symbols, `code_check`, terminal, jobs, LSP,
   autoindex), so a router would have to know which sources exist per platform.
6. **Memory injection is unconditional** (20 facts always in the system prompt) and the file has no maintenance tools.
7. **Old positions are not corrected.** The model gets `file:line` that can be stale after its own edits; only a
   re-read fixes it (K5).
8. **Sub-agents cannot run in parallel** because of one model and a swapped KV cache (S5): any "parallel retrieval
   subagents" in the UIOE has to be engine-side code (no model), or to accept serial execution.
9. **Brand reference in `teacher.c`.** The header says each fala "vem da API de mensagens da Anthropic (Claude)". The
   project rule is that nothing in katana cites other products or engines. Flagged for the owner; **resolved 2026-11-23**: the vendor code moved to `plugins/teacher-anthropic/` and the binary speaks a neutral protocol (`docs/teacher-protocol.md`).
10. **Investigation logic lives in Python** (`tools/findbench/`, `docs/investigation-engine.md`), so its measured pieces
    (activation, confidence calibration) have no C counterpart except `findgraph.c`.

---

## 10. (d) Global state and coupling that hinder one engine and parallelism

Counts are from grep and are approximate (they count definition lines; `static const` tables excluded where noticed).

| Area | Non-const file-level statics | Static buffers inside functions | `g_*` definitions |
|---|---|---|---|
| `src/engine` | ~39 | ~46 | ~8 |
| `src/agent` | ~70 | ~73 | ~94 |
| `src/tools` | ~34 | ~30 | ~45 |
| `src/coder` | ~11 | ~4 | ~38 |
| `src/app` | ~107 | ~35 | ~209 |

Other counts: about 294 distinct `KATANA_*` environment names and about 450 `getenv` calls in `src/`; 25 `die`/exit sites
in agent and tools (`die` is `exit(1)`, `base.c:204`); 99 `setenv/putenv/chdir` call sites.

### D1. One model, one GPU, one session per process

- GPU and CPU compute contexts are file statics: `vkg` (`vk_dense.c:24`), `vkb` (`vk_dense.c:307`), `gpu`
  (`cpu_kernels.c:815`), `vk_moe_out` (`vk_moe.c:201`); worker thread pool in `base.c:393`. The `Model` struct
  (`model.c:124`) holds the KV cache and DeltaNet state, but the surrounding statics make a second `Model` in the same
  process unsafe.
- Sampling and format knobs are globals read by the forward path: `g_top_k`, `g_top_p`, `g_gelu`, `g_spec_tools`,
  `g_vocab_only`, `g_tool_names`/`g_tool_params`/`g_ntool_names` and `g_tool_sys`/`g_agent_sys` (the constrained-call
  tables), so two agents with different tool sets cannot decode concurrently.
- The server serialises generations with a ticket mutex (`server.c:35-50`, `srv_lock`); `serve --agent` allows one active
  agent session and runs it as a child process (`agentapi.c:1-12`). Concurrency today is by processes, never by
  threads or by sessions in one process.
- `agent_regrow` (`loop.c:28-66`) frees VRAM by `execv` of the whole binary after saving the session, passing state through
  `setenv("KATANA_REGROWN")`. The process is the unit of state reset.

### D2. Agent state is spread over file statics, reset by convention

- Loop request state: `rp_sig/rp_res/rp_err/rp_n/rp_fail/rp_refused/rp_stuck` (`loop.c:315`), `bk_key/bk_cnt/bk_n`
  (`loop.c:332`), `rr_sig/rr_step/rr_reps/rr_n/rr_total` (`loop.c:~468`), reset by hand at each request (`loop.c:700`,
  `rr_reset`). A router that runs several routes would need these per route or per call.
- Session state: `g_todo`, `g_todo_touched` (`todo.c`), `g_ct_tasks`/`g_ct_ntasks` (`tasks.c`), `g_ct_seen` (`common.c:140`),
  `g_rk` read-by-difference store (`facts.c`), `g_fc_f`/`g_fc_c`/`g_fc_fail` notebook, `g_undo` (`undo.c:8`), `g_dg` digest
  cache, `g_sess_path`, `g_resume_path`, `g_roots` (granted folders, `tools.c`), `g_trust`, `g_plan_mode`, `g_web`, `g_code`,
  `g_budget`, `g_stop`, `g_busy`, `g_con_rich`. Reset functions exist (`ct_session_reset`, `ct_seen_reset`, `rd_reset`,
  `todo_clear`) and are called by hand in several places (`loop.c:661`); a missed reset leaks state between sessions.
- Large function-local static buffers in `agent()` (`loop.c:536`: `line[16384]`, `text[90000]`, `result[TOOL_WEB+512]`,
  `safe[2*TOOL_WEB+2048]`, `name`, `extra`, `sys[32000]`), `agent_tool_run` (`dispatch.c:29`: `arg[8192]`,
  `content[65536]`), `tool_delegate` (`delegate.c:25`: own `text[90000]`, `result`, `safe`, `xjs`), `tool_digest_in`
  (`delegate.c:157`). Re-entrancy is avoided by a flag (`g_in_sub`, `g_ct_depth`), not by design. At least four separate
  decode loops exist (agent loop, `tool_delegate`, `tool_digest_in`, `ah_summary`/`ah_summary_live`) plus the server and
  the teacher path, each with its own buffers and stop conditions; a UIOE that wants "ask the model a small question"
  has no single `generate(prompt, grammar) -> text` entry point to call.
- Model handle passed through globals: `g_agent_m`, `g_agent_pos` are set before every tool call (`loop.c:844, 902`) so
  that tools that generate text (`delegate`, `digest`) can reach the model and the cache position.

### D3. Process-wide resources used as if they were per-session

- **cwd**: `chdir` is the way the working folder changes (`/cd` in `loop.c:592-594`, `EnterWorktree` `worktree.c:116,170`,
  `devtools.c:512`, `mcp_serve.c:108,168`, `launch.c:1573`), and tools read `getcwd`. MCP and LSP servers are told by
  `mcpx_cwd_sync` (`loop.c:594`). Two sessions or two parallel routes in one process would fight over cwd.
- **stdout/stderr redirection with `dup2` in the parent**: `extend.c:613` (redirects fd 1 to 2 around a command),
  `mcp_serve.c:176-177` (fd 1 becomes stderr, fd 0 becomes /dev/null for the whole process; the protocol uses a saved copy),
  `learn.c:363-374`, `scenario.c:76-93,238`, `launch.c:1670`. The other `dup2` calls (`bash.c:68`, `jobs.c:64`,
  `terminal.c:190`, `mcp.c:284`, `lsp.c`, `reaper.c:58`) are in forked children and are fine. Anything that prints from a
  thread while a parent-side redirect is active is lost or misrouted.
- **stdout is also the UI protocol**: `\x01TOOL`, `\x01TOOLOUT`, `\x01TODO`, `\x01TOKENS`, `\x01ASK`... are written with
  `printf` from many places (`ui_tool_mark`, `ui_toolout`, `sudo.c`, `ask.c`, `tasks.c`, `worktree.c:52`), interleaved with
  model streaming (`ui_live`, `g_ui_live`). A library-style engine would need an event sink instead of `stdout`.
- **stdin** is the input channel, unbuffered on purpose (`setvbuf(stdin, NULL, _IONBF, 0)`, `loop.c:~570`) so that the
  re-exec does not lose lines; `poll(0)` is used to decide whether it is safe to regrow (`loop.c:34`).
- **Signals and exit hooks**: `sigaction` for terminal restoration (`exec.c:115-128`), `SIGINT` swap around commands
  (`exec.c:163-188`), `SIGPIPE` ignored in `lsp.c:197`, `server.c:898`, `mcp_serve.c:179`; `atexit` for `ct_mail_unregister`
  (`loop.c:570`), `lsp_shutdown` (`lsp.c:384`), `job_all_kill` (`jobs.c:73`), `pwfile_remove` (`sudo.c:40`), `lc_cleanup_tmp`
  (`launch.c:1650`). Teardown order across modules is implicit.
- **Environment as configuration and as IPC**: `getenv` is read at the point of use, repeatedly (`rr_on()` on every call,
  `cp_due` per round, `agent_act` per action, 32 `getenv` calls in `loop.c`, 35 in `vk_dense.c`, 17 in `qwen35.c`), so a
  setting cannot differ between two sessions in a process, and tests flip behaviour with environment variables. `setenv` is
  used for state (`KATANA_REGROWN`, `KATANA_REAP_FD`, `KATANA_ALLOWED_TOOLS`, `KATANA_FIRST_MESSAGE`: `loop.c:56`,
  `reaper.c:73`, `launch.c:1339-1351, 1636`).
- **`die()` is `exit(1)`** (`base.c:204`) and is called from module code (`mcp_channel.c` 7 sites, `mcp.c` 4, `locate.c` 2,
  `context.c`, `facts.c`, `loop.c`, `lsp.c`, `project.c`, `teacher.c`). A router component cannot fail soft or time out
  one source without risking the process.

### D4. Single translation unit

`src/katana.c` includes every part, so all `static` names share one namespace. Modules use prefixes (`rp_`, `bk_`, `rr_`,
`fg_`, `cs_`, `sy_`, `ct_`, `mcpx_`, `pfx_`, `ah_`, `cp_`...) as the only isolation, and coupling goes through forward
declarations that depend on include order: `loop.c:527` declares `ct_todo_autofinish` "from `src/tools/tasks.c`",
`dispatch.c:26-35` declares nine functions from `tools/`, `delegate.c`, `table.c` and `messages.c`, `find.c:65` declares
`fg_neighbor_note`, `tools/agent.c` hooks `g_ct_sub_run`, weak symbols for `ct_tool_info` (`toolsearch.c`). The coder side
shows the opposite discipline (`store.c` states it must not know code types and is included before `types.c` so the
compiler enforces it): that is the pattern to copy for a UIOE module.

### D5. What this means for the UIOE

1. Put the UIOE state in one struct (request, interpretation, route record, evidence list, budget) allocated per request
   and passed by pointer; do not add more `g_*`. The existing `rp_/bk_/rr_` arrays are the model to replace, not extend.
2. Keep retrieval outside the model path: the router and fetchers use CPU only and can run on threads or child processes
   (as `katana coder index` already does through `posix_spawn`, `loop.c:461`), because the model, KV cache and GPU
   statics cannot be shared.
3. Do not rely on `chdir` or on `stdout` inside UIOE code; take explicit `root` and an event callback. `Find`/`Symbols`
   already take a root path (`sym_ensure_index(root, ...)`, `coder_lookup(root, ...)`).
4. Add one `generate(model, prompt, grammar, max) -> text` function by extracting the common core of `ah_summary`,
   `tool_digest_in` and `tool_delegate`; the UIOE's optional expander (R17) and sufficiency tie-break need it.
5. Make decision logging lossless and structured before building routes (M4), since the pre-LLM stage today leaves no
   record.
6. Configuration: read `KATANA_*` once into a config struct at session start for UIOE code; leave the legacy `getenv`
   reads alone until a parallel mode is really needed.
