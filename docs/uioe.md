# UIOE: Unified Intelligence Orchestration Engine (design)

Status: design only, 2026-11-23. No code was changed, no model was run and nothing was measured for this
document. Numbers marked "observed" were read from the owner's real sessions (`~/.katana/agente.log` and
`~/.katana/desktop/sessoes/*.json`); numbers marked "estimate" are arithmetic or judgment, not results. Function and
file names were read in the tree at the commit this document is added to.

Related documents: `docs/code-intelligence.md` (the coder module; its sections 5 to 9 are generalized here),
`docs/code-intelligence-architecture.md` (why), `docs/code-index-lifecycle.md` (index levels N0 to N4, crawler,
`index_ensure`, phases and waves), `docs/investigation-engine.md` (what the research measured), `docs/tools.md`,
`docs/mcp.md`, `docs/curriculum.md` and `data/curriculum/v7/README.md` (what training assumes).

## 0. The decision in twelve lines, and the interpretation adopted

The owner's architecture, which this document follows block by block:

```
USER INPUT            any request in natural language
    |
    v
UIOE                  Interpret . Classify . Route . Retrieve . Orchestrate
    |
    v
SOURCES               Filesystem | Web search | Financial APIs | Code repositories | Legal sources | Databases
    |
    v
CONTEXT BUILDER       selects and organizes the relevant data
    |
    v
LLM EXECUTION         receives the request and the prepared context
```

1. **UIOE is the front door, and it runs before the model.** Every request is interpreted, classified and routed
   by C code; the sources are queried; a Context Builder assembles what was found; only then does the model run.
   The model's steps are the expensive resource (1 to 2.5 minutes each, observed); UIOE spends milliseconds to
   save them. In the owner's words: first hit the request, filter, and only then send it to the model.
2. **It is general, not a code feature.** Sources are pluggable connectors. katana already has the filesystem, the
   web, the code repository (the `coder` module), project documents, memory and MCP. Financial APIs, legal sources
   and databases enter only as external processes through MCP (the owner's rule: third parties are plugins).
3. **Five functions, five blocks**: Interpret (entities, language, constraints, references, ambiguity), Classify
   (domain, task kind, complexity, whether retrieval helps, risk), Route (which sources, order, budget, or no
   retrieval at all), Retrieve (connectors, with state and verification), Orchestrate (multi-step pipelines,
   fusion, deduplication, stop).
4. **The router is deterministic**: rules, cue lists, the Portuguese-English knowledge base and index lookups.
   The local model enters only as an optional expander for low-confidence cases, switched off by default.
5. **The Context Builder is a separate, first-class component.** It selects, orders, deduplicates, compresses and
   renders within a token budget, with source, confidence 0 to 100 and what was not found.
6. **One block in the user turn**: `[katana: context ...]` to `[end of context]`, at most 1500 tokens, fixed labels
   (`Question understood as`, `Answer path` or `Findings`, `Evidence`, `Links`, `Not found / limits`, `Next`). The
   system prompt never changes per request, so the prefix cache stays valid.
7. **Injection is conditional.** A packet is injected only when classification says retrieval is useful **and** the
   route confidence is high. A vague, conversational or ambiguous request gets nothing: a bad packet poisons the
   context of a small model.
8. **Inside the loop, the same engine.** `Find`, `Symbols` and `Read` stay as they are for the model and become
   facades of the same UIOE, sharing the state of the request: what the packet showed is not repeated. No new tool.
9. **Verified fact and guess are never mixed.** An item reread in the file carries its confidence number; an
   unchecked item carries `(guess)`. `Not found / limits` is always present and says why, so the model does not
   search again.
10. **Retrieved content is data, never instruction.** Secrets never enter the packet, and an external connector
    never receives the user's text unless the route decided to send it.
11. **Everything is traced** in `~/.katana/uioe.log`; `katana uioe explain` reproduces the decision of a request.
    No measurement campaign now: a small set of manual acceptance scenarios for the owner to run on the desktop.
12. **Off by default**: `KATANA_UIOE=off|pack|full` (`pack`: the pre-LLM packet only; `full`: packet plus unified
    facades), until the owner tries it on the desktop and turns it on.

**Interpretation adopted.** The name and the diagram are the owner's; there is no written specification beyond
them. This document reads the diagram literally: UIOE is the pre-LLM gateway, the sources are connectors behind one
interface, the Context Builder is its own component, and "LLM execution" is the agent loop that exists today
(`src/agent/loop.c`). The decisions marked D1 to D10 below were delegated by the owner on 2026-11-23 ("you decide
which pattern is best") and are recorded where they apply; where the code showed a problem the adjustment is
justified in place.

**Scope of this document.** It designs the five UIOE blocks, the connector interface, the Context Builder and the
interface to the loop. **The complete UIOE has other layers that this document does not design** (environment and
world state, long-term memory, context and token management beyond the packet, execution feedback such as tests and
compilation, permissions, learning from sessions, skills and workflows, delegation, routing between models,
evaluation, change management, and the external domain connectors themselves). Section 11 is a first inventory of
them, organized by the owner's diagram, with what exists today and the size of each gap. A second pass, with the
external reference research other agents are doing now, will turn it into the layered reference model and the gap
analysis using the same diagram as its spine; nothing in section 11 is a closed design.

**What this interpretation leaves out, on purpose.**

- UIOE does not answer the user and does not edit files. It prepares context; the model analyses and acts.
- It does not replace the tools. Every tool still sees everything; the packet is a selection, never a filter.
- It does not decide what the model does. `Next:` is a concrete suggestion with a stop criterion, not a command
  the program enforces.
- It does not write the financial, legal or database connectors. It defines the interface they plug into.
- It is not a service. It is functions in the binary, called in the agent process and from `katana uioe`.

## 1. The problem, measured in the owner's real sessions

### 1.1 One question, three sessions

The owner asked the desktop the same question three times on 2026-11-23 (paraphrased: "I have a project called
greymod in the folder mnt/dados/greymod, with a bridge that links agent terminals to a phone app; how does katana
send the session information to the phone?"). The true answer is a connection that no call or import shows:
`bridge/grey-channel.mjs:61` sends `{ type: "register", ... }` over a WebSocket, `bridge/channels.mjs:33` handles
`m.type === "register"`, stores the link and broadcasts `terminals_changed` at line 37, and
`bridge/server.mjs:112` (`sessionsForPhone`) merges the linked terminals into what the phone lists.

| Session (file in `~/.katana/desktop/sessoes/`) | Wall time | Tool calls | Outcome (observed) |
|---|---|---|---|
| `20261123-080750-311` | 08:07:50 to 08:16:01, 8 min | 6 (2 `Find`, 2 `TodoWrite`, 1 `Read`, 1 `Glob`) | no answer. `Find` ran in the home folder (10.2 s, unrelated results); `Read` of `/home/.../mnt/dados/greymod/README.md` failed; the model repeated the same sentence three times |
| `20261123-101937-867` | 10:19:37 to 10:40:28, 21 min | 15 (2 `Find`, 2 `Grep`, 7 `Read`, 4 `TodoWrite`) | partly wrong answer: names `grey-channel.mjs:61` and `register`, but says `server.mjs` receives it and `phone-link.mjs` forwards it. `channels.mjs` was never opened |
| `20261123-112645-122` | 11:26:45 to 11:53:16, 26.5 min | 18 (3 `Find`, 9 `Read`, 6 `TodoWrite`) | correct answer (lines 61, 33-37, 111-117). The first 8 calls took 11:27:06 to 11:38:56, about 12 minutes, and had not yet reached `channels.mjs` |

Reading the third session call by call (times from `agente.log`):

| # | Time | Call | What happened |
|---|---|---|---|
| - | 11:26:58 | (text) | the model answered the place note in words ("That is a note from the program that runs me...") instead of acting: one step spent |
| 1 | 11:27:06 | `Find` "katana sends session info to the phone" | top result `bridge/README.md`; `grey-channel.mjs` appears as chunk 1-40; `channels.mjs` not shown |
| 2 | 11:28:08 | `Read grey-channel.mjs` (no range) | 168 lines: the map of 7 symbols |
| 3 | 11:30:31 | `Find` same sentence, `mode=refs` | treated the first word as a name: "No definition of katana found ... Used 31 times in 10 files" |
| 4 | 11:31:40 | `Read terminals.mjs` | pure functions, not the link |
| 5 | 11:33:08 | `Read server.mjs` 100+40 | near `sessionsForPhone`, without knowing what feeds it |
| 6-8 | 11:34:59 to 11:38:56 | `Read grey-channel.mjs` 55+40, 95+40, 135+34 | line 61 seen at call 6; the next two ranges were irrelevant |
| 9 | 11:40:29 | `TodoWrite done=1` | error: no list yet |
| 10 | 11:40:29 | `Find` same sentence, `mode=refs` again | ran again with the same result as call 3 |
| 11 | 11:41:55 | `Read channels.mjs` (whole) | first contact with the handler, 15 minutes in |
| 12-18 | 11:46:27 to 11:53:09 | plan, then `Read` 55+40 of `grey-channel.mjs` again, `channels.mjs` 29+40, `server.mjs` 110+40 | rereads of what was already read, one per plan step |

The time between consecutive calls is 1 to 2.5 minutes (observed in the three sessions). `Find` itself took 50 ms.
The engine already had the answer: on the same tree, `Symbols action=flow` (added the same day) prints
`bridge/grey-channel.mjs -> bridge/channels.mjs  permission_request, /channel, register, reply (+1) [confidence 86
high]`. Nobody asked it.

### 1.2 The failures, by kind

| Kind | What was observed | Where it comes from in the code |
|---|---|---|
| **Routing** | The model chose `mode=refs` for a sentence, twice. It read a whole file to "look around". It never called `Symbols flow`, the one query that holds the answer; `Symbols` is not even in the core tool list (`agent_build_sys` in `src/agent/prompt.c`: listed by name only, reached through `ToolSearch`) | The choice of engine is the model's. `ct_findtool` (`src/tools/find.c`) decides channels inside `Find` only (`fd_query_kind`, `fd_qflags`, `fd_is_phrase`), and `Find` knows nothing of `flow`, docs, memory |
| **Routing** | In session 1 the question went to a `Find` rooted at the home folder for 10 s | Fixed since (`ct_cwd_is_home`, `place_note`), by two special cases, not by a router |
| **Connection** | `Find` returned six candidates and no link between them. `grey-channel.mjs` and `channels.mjs` talk through a string; the file graph of `fg_build` (`src/tools/findgraph.c`) has no such edge, and `links:` (commit `cf9916e`, `sym_find_note` in `src/coder/symtool.c`) is printed only for the symbol holding the best line of the first three code results | The engines are connected to the model one at a time; nothing composes "candidates -> key links -> the other end" |
| **State** | `grey-channel.mjs` was read five times in session 3 (map, 55, 95, 135, 55 again); the same `Find` ran twice | The repeat table of `rr_check`/`rr_after` (`src/agent/loop.c`) is cleared by any tool outside `loop_plan_readonly`, and `TodoWrite` is not in that list, so every plan update erased the memory of what was read. The read cursor (`rc_*`) and shown ranges (`rs_*` in `src/tools/common.c`) survive, but nothing uses them to say "you already have lines 55-94" |
| **State** | The plan steps were "read file A, read file B, read file C": the plan restated reads that were already done | `loop_plan_note` asks for a plan by call count; it does not know the request was a question already half answered |
| **Verification** | Session 2 stated that `server.mjs` receives the registration and `phone-link.mjs` forwards it. Neither was read at those lines | Nothing checks a claim against what was actually delivered to the model. The verification that exists is inside single queries (`sy_verify`, `fl_verify`) |
| **Budget** | 26.5 minutes for a question whose evidence is three ranges of about 10 lines | Each model step costs minutes; the engine's cost is milliseconds and is not spent |
| **Contract** | One step was spent answering a `[katana: ...]` note in words | A note that needs no reply still produces a turn. Not a UIOE defect, but the packet must be worded so that it invites a tool call or an answer, never an acknowledgment |

The older baseline agrees: on the mandatory question of `docs/investigate.md` the agent made 8 calls, read 11914
input tokens in 238 s and failed, never looking at how the bridge lists sessions.

### 1.3 What already exists and works

The pieces are present; what is missing is the layer that composes them.

- `Find` decides channels by the shape of the query (`fd_query_kind`: quoted, identifier, natural), demotes docs and
  tests unless the question is about them (`fd_qflags`, `fd_is_test`), skips what the model already saw
  (`rs_cover`), pages on a repeated query (`fd_same_query`), prints `called by:`/`calls:`/`links:` and a `next:`
  line (`fd_next_steps`), and says what was not found (`fd_coder_pre`, `fd_coder_post`).
- `Symbols` (`sym_tool_run`) exposes `def`, `refs`, `callers`, `callees`, `uses`, `outline`, `at`, `deps`, `impact`,
  `flow`, each result with `[confidence N level; how]` from the cascade (`sy_cascade`, `sy_conf`, `fl_pair`).
- `Read` keeps a cursor per file and, for a large file, shows the windows that match what is being looked for
  (`rd_best_windows` with `intent_get()`, which returns the last `Find` query or the user's request).
- The loop adds a place note (`place_note`, `ff_resolve_missing` in `src/agent/locate.c`), selects project
  documents (`ds_select` in `src/agent/docsel.c`), starts indexing (`autoindex_dir`), blocks identical reads
  (`rr_check`), stops insisting on failures (`rp_*`, `bk_*`), and asks for a plan (`loop_plan_note`).
- Memory (`tool_recall`/`tool_remember`/`mem_tail` in `src/agent/tools.c`), session notes (`katana knowledge search`,
  `src/app/knowledge.c`), the facts notebook (`src/agent/facts.c`), the web (`tool_web_search`, `ct_webfetch`), MCP
  (`mcp_call`, cards) and skills (`src/tools/skill.c`) each have their own entry and their own output.

## 2. Principles

1. **The model's step is the unit of cost.** A choice is good when it removes a model step, and suspicious when it
   adds tokens without removing one. A 1500-token packet costs roughly 25 s of reading (arithmetic quoted in
   `docs/code-intelligence.md` 6.3, not measured); it must replace at least one step.
2. **Before the model, not beside it.** The main case is the pre-LLM pass. A tool call inside the loop is the same
   engine called again for a narrower question.
3. **Deterministic and explainable.** The same request on the same sources gives the same route and the same
   packet. Every decision has a rule number that appears in the trace.
4. **An answer path, not a pile of search results.** Two to five numbered steps with exact locations; for code the
   steps are a connection (who sends, who handles, who calls).
5. **Verified fact and guess are different things**, and the packet shows which is which. A fact's confidence
   (0 to 100, from the search cascade: exact, derived, probable) says "is this real"; it never absorbs the other
   doubt, "is this what the user asked about", which is the route confidence and decides whether to inject at all.
6. **Absence is information, with its reason.** Not found because it is outside the indexed set, in another
   project, in an ignored folder, or because the index is partial: each is said, so the model does not repeat the
   search.
7. **A weak packet is worse than no packet.** When retrieval is weak nothing is injected, or only
   `Not found / limits` and `Next`.
8. **Degrade, do not fail.** No connector is mandatory. A missing, slow or partial source is skipped and named.
9. **Content is data.** File text, web pages, database rows, MCP results and memory are quoted and can never sit
   in a line the model is told to obey.
10. **Nothing hidden, nothing repeated.** Everything cut for space is named; nothing the request already contains
    or the request state already showed is sent again.
11. **Fixed vocabulary.** The same labels and delimiters in every packet, every domain, every target model: that
    is what makes the format trainable and the model's reading predictable.
12. **No AI in the core, no embeddings, no external service in the core.** Third-party programs are plugins in
    separate processes. Risky features are born switched off, each with one environment variable.

## 3. Architecture, layer by layer

### 3.0 The diagram, and what katana has in each block

| Block of the owner's diagram | What it does | What katana has today (file, function) | What is missing | Section |
|---|---|---|---|---|
| **USER INPUT** | any request in natural language, from any front end | the line read by the loop (`src/agent/loop.c`), from the console, the desktop, `katana -p`, `katana serve --agent` and MCP channels (`mcpch_take`); `intent_set_user` keeps the text for `Read` | one entry function that every front end passes through | 3.2 |
| **UIOE: Interpret** | entities, language, intent, constraints, references to places, files, people and dates, ambiguity | pieces: `place_note` (a cited folder), `fd_query_kind` (quoted, identifier, natural), the normalizer and request-word lists of `src/coder/text.c`, the knowledge base (`src/coder/kb.c`) | one interpreted-request object; dates, names, amounts, constraints; ambiguity as a result | 3.3 |
| **UIOE: Classify** | domain, task kind, complexity, whether retrieval helps, risk | `fd_qflags` (docs or tests), `loop_plan_readonly`, `ct_cwd_is_home`; the design of `docs/code-intelligence.md` 5.2 (not built) | everything beyond code and documents | 3.4 |
| **UIOE: Route** | sources, order, parallelism, budget; or "do not retrieve" | inside `ct_findtool` only (channels of one tool); `autoindex_dir` (prepare the index of a folder) | a route plan across sources; route confidence; the decision not to retrieve | 3.5 |
| **UIOE: Retrieve** | query the connectors, with state and verification | per-tool state (`rs_*`, `rc_*`, `rr_*`), per-query verification (`sy_verify`, `fl_verify`) | shared request state, one verification pass, a cache | 3.6 |
| **UIOE: Orchestrate** | multi-step pipelines, dependencies between sources, retries, fusion, deduplication, stop | `fd_next_steps`, `sym_find_note` (`called by`, `calls`, `links` under a Find result), tiers and rank fusion in `cs_search_core` | pipelines per task kind; an evidence graph; stop rules | 3.7 |
| **SOURCES** | Filesystem, Web search, Financial APIs, Code repositories, Legal sources, Databases | filesystem (`Read`, `Grep`, `Glob`, `locate.c`), web (`src/agent/web.c`, `webnav.c`), code repository (`src/coder/`: search, symbols, flow), documents (`docsel.c`), memory (`tools.c`), earlier sessions (`src/app/knowledge.c`), MCP client (`src/agent/mcp.c`) | one connector interface; financial, legal and database connectors (external, through MCP) | 3.8 |
| **CONTEXT BUILDER** | selects and organizes the relevant data | two fixed notes appended to the user turn: `ds_select` (600 tokens of document lines) and `place_note` | the component itself: selection, order, deduplication, compression, budget, rendering per target model | 3.9 |
| **LLM EXECUTION** | receives the request and the prepared context | the agent loop, its tools and its loop notes (`rr_check`, `rp_*`, `loop_plan_note`, `fg_neighbor_note`) | the interface from the Context Builder; tools as facades of the same engine | 3.10 |

Section 1.3 lists the existing pieces in more detail. The point of the table: every block has fragments, none has
an owner. UIOE is the owner.

### 3.1 Place in the tree and the dependency rule

```
  front ends (console, desktop, -p, serve --agent, MCP channel)
        |  request text
        v
  src/agent/loop.c ---- uioe_prepare(session, text) ---------------------------+   (pre-LLM, the main case)
        ^                                                                       |
        |  [katana: context ...] block                                          v
  src/uioe/ctxb.c  <--- evidence graph ---  src/uioe/  interpret -> classify -> route -> retrieve -> orchestrate
  (Context Builder)                               |  state (request, session) | budget | cache | trace
        ^                                         v
        |  same engine, narrower question   connectors (3.8)
  tools Find / Symbols / Read (facades)       in process: code.*  file.*  docs.*  memory  history  web
                                              external:   mcp:<server> (databases, legal, financial, ...)
```

- `src/uioe/` depends on `src/coder/` (direct calls through the structured API of 3.8.4) and on nothing in
  `src/agent/`.
- Connectors that live in the agent (memory, web, MCP, skills, docsel, file tools) are **registered at start by
  `src/agent/uioe_agent.c`** as function pointers. The UIOE core never includes an agent file, so `katana uioe ask`
  works with no model loaded and the `katana-coder` build rule of `docs/code-intelligence.md` 1.3 is not broken.
- Include position in `src/katana.c`: after `coder/coder.c` and before `agent/mcp_serve.c` and `agent/loop.c`.
  `src/tools/find.c` and `read.c` come earlier in the unit and reach UIOE through forward declarations, the way
  `find.c` already declares `coder_lookup` and `sym_find_note`.
- The Context Builder is its own file (`src/uioe/ctxb.c`) with its own interface (3.9): it receives an evidence
  graph and a target profile and returns text. It knows nothing about connectors, and UIOE knows nothing about
  prompt formats.

### 3.2 USER INPUT

Any request in natural language. Today the loop receives it as `line` and, before the model runs, already applies
pre-hooks in this order: the undo note, `intent_set_user(line)`, `ds_select` (docsel), `place_note` plus
`autoindex_dir` for cited folders, then images (`vis_take`) and the plan-mode note. UIOE takes the same position and
**the original text, before any note is appended**, through one function:

```c
/* src/uioe/api.c: a unica entrada do caso principal (pre-LLM) */
int uioe_prepare(UioeSession *s, const char *request, const UioeTarget *target, char *block, size_t cap, UioePackStats *st);
/* devolve 0 = nada a injetar (block vazio), 1 = bloco pronto. Nunca falha: erro interno vira 0 e uma linha no traco. */
```

Front ends do not change: the console, the desktop, `-p`, the server and MCP channels all reach the loop's request
preparation, and that is where the call sits. A request that arrives with attachments (an image, a pasted file) is
interpreted on its text only; the attachment is the model's.

### 3.3 UIOE block 1: Interpret

The output is one object, the interpreted request (text form in 4.3):

```c
typedef struct {
    char text[2000];                    /* o pedido original, sem as notas [katana: ...] */
    char lang[4];                       /* "pt", "en", "?" (so o que text.c reconhece) */
    UioeEntity ent[32]; int nent;       /* ver a tabela abaixo; cada uma com tipo, texto, como foi resolvida e confianca */
    uint8_t rel;                        /* bits de relacao: quem chama, onde e definido, usa, envia/recebe, funciona x nao funciona, negacao */
    UioeConstraint con[8]; int ncon;    /* restricoes: periodo, lugar, formato pedido, "sem alterar", "so em X", limite */
    char root[1100], prefix[512];       /* onde procurar (pasta de trabalho, pasta citada, `path` da chamada) */
    double gap;                         /* fracao dos termos informativos ausentes dos indices consultados */
    uint8_t ambiguous; char amb[200];   /* 1 e o motivo: duas leituras com evidencia parecida, referencia sem alvo ("isso", "o outro") */
    uint8_t origin;                     /* UO_REQUEST (pedido do usuario), UO_TOOL (chamada do modelo), UO_CLI */
    uint8_t inherits;                   /* continua a investigacao aberta do pedido anterior (3.6.2) */
} UioeAsk;
```

| Entity kind | Recognized by (rules, no model) | Resolved against |
|---|---|---|
| symbol | identifier shape (underscore, camelCase, `::`, `.`, call parentheses) or an exact entry of the name table | the symbol index (cascade: exact, derived, probable) |
| path, file, folder | a token with `/` or a known extension; the word after pasta/projeto/folder/project/repo (the `place_note` rule) | the filesystem; `ff_resolve_missing` for a place that does not exist from the working folder |
| quoted literal, error text | quotes (straight, curly, back); `error:`, `Exception`, a line with `:` and digits | literal search |
| text key | an exact entry of the key-name section (message type, event, route, config key, subcommand) | `code.flow` |
| environment variable | `ALL_CAPS_WITH_UNDERSCORE` | `code.flow` class `env` |
| URL, e-mail | scheme or `@` shape | the web connector (only if the route decides) |
| date, period | ISO and local date shapes; today, ontem, yesterday, "ultimo trimestre", "last week", month names, "em 2025" | normalized to a closed interval from today's date (the date `agent_env` already prints) |
| amount, number with unit | digits with currency sign or code, percent, unit | kept as a constraint or a filter value |
| proper name (person, organization, product) | a capitalized word or run that is not sentence-initial, not in the code index and not in the stop lists; a name after "cliente", "empresa", "client", "company", "contrato com" | memory facts, document text, connector parameters. Recognition is by shape, so it is marked `(guess)` until a source confirms it |
| concept | an informative word left after the above | the knowledge base (`src/coder/kb.c`: Portuguese and English terms of a concept) |

Honest limit: dates, amounts and names are recognized by shape in Portuguese and English only. A name written in
lower case, or a period phrased unusually, is missed; the request then has fewer constraints and the route is
weaker, never wrong by invention.

**Ambiguity is an output.** `ambiguous = 1` when: a reference has no target ("isso", "o outro", "that file") and
the session has no open investigation; a name resolves to several unrelated things with similar scores (two
projects called the same, a symbol defined in unrelated modules and no path hint); or two domains tie in 3.4. An
ambiguous request is not retrieved for (rule G3 in 3.5.3); at most it gets the two readings as a `Not found /
limits` line so that the model can ask the user with the right options.

**Two origins, one interpreter.** A user request (`UO_REQUEST`) is the main case. A tool call the model made
(`UO_TOOL`: `Find`, `Symbols`, `Grep`, `Read`) is interpreted by the same code: the call's own arguments decide
first (a `Symbols action=callers` needs no guessing), and a `Find` with a natural sentence is interpreted like a
request, with the user request of the current turn as context for what the query omits. This is where "the model
called `mode=refs` with a sentence" stops being an error.

#### 3.3.1 The rules

The steps below are the interpreter of `docs/code-intelligence.md` 5.2 (steps 1 to 4, kept), extended. Step 5 is
the task-kind table that Classify (3.4) uses; it is printed here because its cues are found in the same pass.

Applied in order (`src/uioe/interpret.c` and `classify.c`; no model; microseconds):

1. **Tokenize** with the coder normalizer (`src/coder/text.c`: folding, Portuguese and English stemming, stop words
   `g_ct_pt_stop`/`g_ct_en_stop`, request words `g_ct_pt_req`/`g_ct_en_req`), keeping original spellings and quoted
   strings whole.
2. **Entities by lookup, never guessed.** Identifier shape or an exact entry of the name table -> symbol; a token
   with `/` or a known extension, matched by path suffix -> path; quoted text of two or more words or an error shape
   -> literal; an exact entry of the key-name section (0x207) -> text key, with its class (`msg`, `event`, `route`,
   `env`, `config`, `cmd`); `ALL_CAPS_WITH_UNDERSCORE` -> environment variable; `http(s)://` -> URL; a folder that
   `place_note` resolves -> root.
3. **Generic words dropped** (request words, stop words, terms in more than 15% of the files), kept for the trace.
4. **Relation bits**: who calls / quem chama; where defined / onde fica; uses / usa / depende; sends, receives,
   emits, handles, reaches, arrives / envia, recebe, manda, chega, trata, escuta (new: the direction of a flow);
   negation near a verb; a working and a failing sibling.
5. **Kind**, first match wins. Each row is a numbered rule so the trace can say which one fired.

   | # | Kind | Strong cues (Portuguese and English) | Also when |
   |---|---|---|---|
   | R10 | `UI_NONE` | a greeting, "ok", "continue", "sim, pode", a shell command to run, a request about an attached file, fewer than 12 characters | no informative term and no entity |
   | R20 | `UI_WEB` | a URL; latest, news, current version, price, weather, "na internet", "search the web" | the only entities are URLs |
   | R30 | `UI_HISTORY` | "what did we", "o que combinamos", "da ultima vez", "you said", "lembra", "remember", "ontem", "last session", "decidimos" | first or second person plus a past tense and no code entity |
   | R40 | `UI_ERROR` | "por que ... nao", "why ... not", erro, error, falha, crash, trava, exception, stack trace, "nao aparece", "stopped working" | a literal with an error shape; the negation bit; a working/failing pair |
   | R50 | `UI_IMPACT` | "o que quebra", "what breaks", impacto, "se eu mudar", "if I change", "is it safe to", "quem depende", "posso remover" | a symbol entity plus an imperative of change (`change = 1`) |
   | R60 | `UI_CONFIG` | config, configuracao, setting, opcao, variavel de ambiente, env, flag, parametro, default, porta, timeout, "where is ... set" | an environment-variable or config-key entity |
   | R70 | `UI_FLOW` | "como ... envia / chega / funciona", "how does ... reach / send / work", fluxo, passo a passo, "o que acontece quando", entre, "from ... to" | the send/receive relation bits; two entities in different files |
   | R80 | `UI_USAGE` | "quem chama", "who calls", "onde e usado", "where is ... used", "onde esta definido", definition, references | exactly one symbol entity and a relation bit |
   | R90 | `UI_HOWTO` | "como instalo", "como uso", "como faco para", "how do I", install, setup, usage, tutorial, guide, README (the `fd_qflags` list) | no code entity and an imperative about using, not changing |
   | R95 | `UI_OVERVIEW` | "o que e este projeto", "what does this project do", estrutura, arquitetura, visao geral | no entity |
   | R99 | `UI_LOCATE` | onde, where, qual arquivo, "find the" | **default** when at least one entity or two informative terms exist; `sure = 0` |

   Ties: `UI_ERROR` over `UI_IMPACT` over `UI_FLOW` over `UI_USAGE` (the richer plan contains the poorer). A wrong
   kind costs budget, not correctness: every pipeline returns evidence and none returns a conclusion.
6. **Scope.** Root = the folder `place_note` resolved, else the `path` of the call, else the working folder. If the
   working folder is the home folder and no root was resolved, only non-code engines run (the `ct_cwd_is_home`
   rule, generalized).
7. **Vocabulary gap** = fraction of informative non-entity terms with document frequency 0. A request in a
   language the normalizer does not know has `lang = "?"`; rules 2 and 6 still work, the cue lists do not, and the
   kind falls to R99 with `sure = 0`.

The cue lists are compiled in (Portuguese and English) and contain no word of any project. They are data tables,
one per kind, so adding a cue is one string.

### 3.4 UIOE block 2: Classify

Five independent answers, each by rules, each with a rule number in the trace.

```c
typedef enum { UD_CODE, UD_FILES, UD_DATA, UD_LEGAL, UD_FINANCE, UD_RESEARCH, UD_MEMORY, UD_SYSTEM, UD_CHAT } UioeDomain;
typedef struct {
    UioeDomain domain[3]; int ndomain;  /* um pedido pode cruzar dominios: ate 3, o principal primeiro */
    UioeKind kind; uint8_t sure;        /* tipo de tarefa (tabela de 3.3.1, passo 5) e se uma regra forte casou */
    uint8_t change;                     /* o pedido quer alterar algo: os planos trazem corpos e impacto */
    uint8_t complexity;                 /* UX_LOOKUP (um salto, uma fonte), UX_MULTIHOP (varios saltos, uma fonte), UX_MULTISOURCE */
    uint8_t retrieve;                   /* 0 nao recuperar, 1 recuperar, 2 so localizar o lugar (pasta citada) */
    uint8_t risk;                       /* bits: UR_CHANGES_FILES, UR_RUNS_COMMANDS, UR_LEAVES_MACHINE, UR_SENSITIVE, UR_DESTRUCTIVE */
    uint16_t rule_domain, rule_kind, rule_retrieve;
} UioeClass;
```

**Domain.** First by what resolved, then by cues. A wrong domain costs a source that returns nothing; it cannot
produce a false item.

| Domain | Decided by what resolved | Cue words (Portuguese and English), used when nothing resolved |
|---|---|---|
| `code` | a symbol, text key or source path resolved in a code index; the root is a project | funcao, classe, bug, compilar, endpoint, function, class, build, commit, stack trace |
| `files` | a path or folder resolved that is not source code; a document name | arquivo, pasta, planilha, pdf, documento, file, folder, spreadsheet |
| `data` | a table, column or named query of a registered database connector matched | tabela, registros, quantos, total, soma, media, consulta, table, rows, how many, sum, average, query |
| `legal` | a registered legal connector's term list matched; a law or article shape ("art. 5", "Lei 13.709") | contrato, clausula, lei, artigo, prazo legal, jurisprudencia, contract, clause, statute, case law |
| `finance` | a ticker, currency pair or index name of a registered financial connector matched; an amount with a period | cotacao, cambio, juros, indice, acao, preco, quote, exchange rate, interest, index, stock, price |
| `research` | a URL; nothing local resolved and the request asks about the world | o que e, quem e, noticias, ultima versao, what is, who is, news, latest |
| `memory` | first or second person with a past tense and no other entity | combinamos, decidimos, lembra, da ultima vez, we agreed, remember, last time |
| `system` | a command to run, a program, a process, a port | instale, rode, execute, reinicie, install, run, restart, kill |
| `chat` | none of the above; a greeting, thanks, an opinion, a rewrite of text the user pasted | - |

A domain whose connector is not registered is still classified (the trace shows it) and becomes a limits line:
"this looks like a database question; no database connector is installed".

**Task kind.** The table of step 5 in 3.3.1 (`UI_LOCATE`, `UI_FLOW`, `UI_IMPACT`, `UI_USAGE`, `UI_CONFIG`,
`UI_ERROR`, `UI_HOWTO`, `UI_HISTORY`, `UI_WEB`, `UI_OVERVIEW`, `UI_NONE`), written for code and documents, plus
one general kind for the other domains: **`UI_FINDINGS`** (answer a question of fact from one or more sources:
"how much", "which", "does the contract allow", "what is the rate"). In non-code domains every retrieval request is
`UI_FINDINGS` unless a specific cue matches.

**Complexity.** `lookup`: one entity, one relation, one source (where is X defined). `multi-hop`: a relation that
must be followed (flow, impact, error). `multi-source`: two or more domains, or one domain whose pipeline needs a
second source to qualify the first (a figure from a database and the rule that applies to it from a document).
Complexity sets the budget (3.5.2) and the ceiling of the packet.

**Does retrieval help?** (`retrieve`)

| Rule | Condition | Result |
|---|---|---|
| C1 | kind `UI_NONE`: greeting, "ok", "continue", a follow-up with no informative term and no open investigation | 0 |
| C2 | domain `chat`, or a request whose material is in the request itself (pasted text or code longer than the question; "rewrite this", "translate this") | 0 |
| C3 | a pure creation request that names nothing existing ("write a script that ...", "create a function to ...") | 0; with a resolved root, 2 (tell the model where it is) |
| C4 | domain `system` with a command to run and no question | 0 |
| C5 | the request cites a place that had to be located (`place_note`'s case) and nothing else qualifies | 2 |
| C6 | at least one entity resolved, or two informative terms present in an index of the routed sources | 1 |
| C7 | none of the above | 0 |

**Risk.** Set from the verbs and the domain, used by Route and by the policy of each connector (3.8.5):
`changes files` (imperatives of change), `runs commands`, `destructive` (apagar, remover, delete, drop, reset),
`leaves machine` (the route would send terms to a network source), `sensitive` (the request or the resolved files
touch secrets, credentials, personal data folders, or a domain flagged sensitive by its connector: financial,
legal). Risk never blocks the model (the owner's rule: the agent acts without confirmation); it limits what UIOE
itself does unasked.

### 3.5 UIOE block 3: Route

Route turns (interpreted request, classification) into a **route plan** (text form in 4.4): which connectors, in
what order, with which query each, what may run in parallel, the budget, and the decision to inject or not.

```c
typedef struct {
    uint16_t pipeline;                  /* tabela de passos escolhida (3.7.2) */
    UioeStep step[24]; int nstep;       /* {capacidade, conector escolhido, entrada, condicao, limite, se vazio, depende de} */
    int deadline_ms, token_ceiling;
    uint8_t parallel[24];               /* grupo: passos do mesmo grupo podem rodar juntos (processos; 3.11.2) */
    uint8_t sends_out[24];              /* este passo manda texto para fora da maquina: quais termos (nunca o pedido inteiro por padrao) */
    int confidence;                     /* confianca da rota, 0-100 (3.5.3) */
    uint8_t decision;                   /* UG_NONE (nao injeta), UG_LIMITS (so Not found / limits + Next), UG_PACKET */
    uint16_t rule;                      /* regra G* que decidiu */
} UioeRoute;
```

#### 3.5.1 Choosing sources and order

1. The pipeline is chosen by (domain, task kind) from a static table (3.7.2).
2. Each step names a **capability** (3.8.3), not a connector. The registry returns the available connectors that
   have it, cheapest first. A missing connector is a degradation, not an error.
3. **Order**: local before remote, exact before fuzzy, cheap before expensive. A step that depends on another's
   result (the keys of the files the search returned; the clause that applies to the figure the database returned)
   is marked `depends on`.
4. **Parallelism**: steps with no dependency between them and cost class `process` or `network` form a group and
   are started together; in-process index steps run in sequence (they take milliseconds; see 3.11.2).
5. **What leaves the machine** is decided here and nowhere else: a step to a network or external connector carries
   the exact terms to be sent (entities and informative terms, never the raw request unless the connector's policy
   says `question`), and runs pre-LLM only if the connector's policy allows it without asking (3.8.5).
6. **The local model as expander** (optional, off): only when the kind is not `sure` or `gap >= 0.5`, one
   constrained generation proposes terms (`docs/code-intelligence.md` 5.3). It never chooses the route.

#### 3.5.2 Budgets, deadlines and breakers

| Budget | Default | Variable | On overflow |
|---|---|---|---|
| Pre-LLM pipeline, wall clock | 400 ms | `KATANA_UIOE_MS` | jump to P6; header says which stages were cut |
| Facade pipeline (the model asked) | 1500 ms | `KATANA_UIOE_TOOL_MS` | same |
| Waiting for a cold or changing index | 2 s cold, 300 ms changes (the lifecycle rule 7) | lifecycle variables | continue with what exists; the status line of lifecycle J.2 |
| One connector call | the connector's `default_ms` (index 50, files 300, process 3000, network 8000, model 6000) | - | `UR_TIMEOUT`; the connector's breaker counts it |
| Expansion fan-out | 5 anchor files, 3 anchor symbols, 4 hops, 25 impact nodes | - | counted in limits |
| Packet | 1500 tokens, 4 bytes per token (the `docsel` rule); hard maximum 3000 | `KATANA_UIOE_TOKENS` | trimming order of 4.6.3 |
| Verification | 40 ranges, 150 ms | - | "not reread" on the rest |
| Context left | the packet never takes more than 25% of the free context (`c->ctx - pos`, as `g_budget` does for tool results) | - | ceiling lowered for this request |

**Circuit breaker**, per connector and per session: after 2 consecutive `UR_TIMEOUT` or errors the connector is open
(skipped) for the next 5 requests, then half-open (one trial call with half its deadline). The state is printed by
`katana uioe connectors` and each skip is a trace line. A skipped connector always produces a limits line ("web: not
consulted, it failed twice in this session").

**Degradation ladder**, in the order a pipeline falls:

1. Index complete and fresh: the pipeline as written.
2. Index partial (level or prefix behind): same pipeline; completeness claims say "so far"; `Not found` uses the
   partial form.
3. No N3 keys: `UQ_LINKS` steps are skipped; the limits say "text-key links are not indexed here".
4. No N2 symbols: `UQ_DEF`/`UQ_CALLERS` fall to `code.search` exact channel plus `file.grep`; confidence at most
   derived.
5. No index at all (read-only home, damaged store, a folder never visited and too large to index in the budget):
   `file.glob`, `file.grep` and `file.windows` only, K = 3, and the header says the search was by scanning files.
6. Nothing usable: no packet. The tools still work; the trace records why.

#### 3.5.3 Route confidence and the decision to inject (D2)

Route confidence answers one question: **is the program sure enough of what it understood and found to put it in
front of the model?** It is a deterministic score, computed twice: before retrieval (to decide whether to retrieve
at all) and after (to decide whether to inject). The weights are provisional and uncalibrated.

| Signal | Effect |
|---|---|
| start | 50 |
| the task kind matched a strong cue (`sure`) | +15 |
| at least one entity resolved at the exact or derived step of the cascade | +20 |
| the scope resolved to a known root whose index is complete at the level the pipeline needs | +10 |
| `gap >= 0.5` (most informative words are absent from the indexes) | -20 |
| two domains or two task kinds tie; `ambiguous` | -25 |
| a connector the pipeline needs is unavailable or partial | -10 each, at most -20 |
| after retrieval: a verified path or finding with confidence >= 75, reached by structure (not by word rank alone) | +20 |
| after retrieval: independent sources agree (a document line names the files the path connects) | +10 |
| after retrieval: nothing above confidence 45, or only word-rank candidates | -25 |

| Rule | Condition | Decision |
|---|---|---|
| G1 | `retrieve = 0` | no retrieval, no packet, state untouched |
| G2 | confidence before retrieval < 35 | no retrieval, no packet |
| G3 | `ambiguous` | no packet; at most the limits form below, naming the two readings |
| G4 | confidence after retrieval >= 70 | `UG_PACKET`: the full block |
| G5 | 40 to 69 **and** there is something authoritative to say that saves a step: a named thing that does not exist in a complete scope, a cited place that was located, an index that is partial, a source that is not installed | `UG_LIMITS`: `Question understood as`, `Not found / limits`, `Next` only |
| G6 | otherwise | no packet. The model works as today, and the same engine answers when it calls `Find` |

`KATANA_UIOE_GATE` (default 70) moves the G4 threshold for experiments. What the gate protects against is the
measured fact that word-only candidates are right at rank 1 about 4% of the time (`docs/investigation-engine.md`
3.3): a packet built on that alone never passes G4.

### 3.6 UIOE block 4: Retrieve

Retrieve executes the route's steps against the connectors. Three things make it more than a loop of calls: every
result is **verified** before it can be shown, the **state** of the request and of the session decides what is new,
and a **cache** keeps repeated questions free.

#### 3.6.1 Verification

Everything that goes into the packet passes `src/uioe/verify.c` in state P5.

| Check | How | Effect |
|---|---|---|
| The line still says it | reread the file range; the symbol name, key text or literal must occur at the cited line, +-3 lines (the tolerance `sy_verify` uses) | `UV_OK`: +4 (the existing bonus). Moved within the tolerance: line corrected. Not found: `UV_STALE`, -25, and the item says "line not verified: the file changed since it was indexed" |
| The file changed since indexed | size and mtime against the store record; content hash when cheap | `UV_STALE` on every node of the file; the pipeline re-parses that one file when the lifecycle print-time check (phase 4, `fresh_check`) exists, else falls back to `file.grep` for the names involved |
| The file is gone or unreadable | `stat`/open | node dropped; `Not found / limits` gets one line |
| Both ends of a link | both lines reread (`fl_verify` does this per occurrence) | a link with one unverified end is shown only under `Evidence`, never in the Answer path |
| A document line | reread; the heading it sits under is attached | stale documents are still shown, with their file date |
| A memory fact or history episode | cannot be verified; if it names a file or symbol, that name is looked up | "names `foo.c`, which exists" or "names `foo.c`, not found in the indexed set"; confidence capped at 60 |
| A web page | not verified | confidence capped at 40, the URL and fetch time shown |
| Optional: language server | `lsp.confirm` on the edges of the Answer path only, when switched on | +3, `how` gains "confirmed by a language server" |

Verification is bounded: at most 40 file ranges reread per request, each at most 200 lines, under a 150 ms budget.
What was not verified for lack of budget is printed with "not reread".

A second, later verification belongs to the loop and is listed in section 11 (execution feedback): checking the
model's *answer* against the packet (the citation check of `docs/code-intelligence.md` 8.0). The orchestration
core only guarantees that what it handed over was true when it was handed over.

#### 3.6.2 State: per request and per session

One structure replaces the scattered globals; phase U0 moves them behind accessors without changing behaviour.

```c
typedef struct {                       /* um pedido do usuario: nasce em rr_reset, morre no proximo pedido */
    uint64_t id; UioeAsk ask; UioeClass cls; UioeRoute route;
    UioeRange shown[256]; int nshown;  /* faixas apontadas pelo Find ou pelo pacote (hoje g_rs com how=1) */
    UioeRange read[256]; int nread;    /* faixas entregues pelo Read (g_rs com how=2) e o cursor por arquivo (g_rc) */
    uint32_t dropped[64]; int ndropped;/* nos que o modelo abriu e nao citou, ou que a verificacao derrubou */
    UioeHyp hyp[8]; int nhyp;          /* hipoteses abertas: entrada, caminho, o que falta confirmar */
    UioeGap gap[16]; int ngap;         /* lacunas: o que se procurou e nao se achou */
    UioeCallRec call[128]; int ncall;  /* chamadas do pedido: assinatura, resultado, falhou (hoje rr_* e rp_*) */
    char last_query[400]; uint32_t last_terms[64]; int nlast;   /* paginacao do Find (g_fd_lq, g_fd_lastq, g_intent_find) */
    int packet_tokens, model_steps;
} UioeRequest;
typedef struct {                       /* a conversa: zera em /clear e ct_session_reset */
    UioeRequest cur; UioeReqSummary past[8];
    UioeNode *graph; int ngraph;       /* nos mostrados ou lidos nos ultimos pedidos (3.7.3) */
    UioeBreaker brk[48]; UioeCache cache;
    char roots[4][1100]; int nroots;   /* pastas em uso (hoje g_place_dirs e a pasta de trabalho) */
} UioeSession;
```

How the state feeds what exists:

- **`Read` with intent.** `rd_best_windows` asks `uioe_read_terms(path)` instead of `intent_get()`: the terms of
  the current intent plus the names of the evidence nodes in that file (the key `register`, the symbol `onLink`).
  A `Read` of a file with no range, when the packet already holds verified nodes in it, answers with those windows
  first and the map after. A `Read` of a range fully delivered in this request keeps today's behaviour (the cursor
  note), and the note names what in the evidence is still unread.
- **`Find` with pagination.** `shown` and `read` are what `rs_cover` consults today; UIOE keeps the rule (a result
  more than half seen is skipped and listed under "already shown"). A repeated query continues from the next
  candidates of the *same* ranked list kept in the request (no second ranking), and from the next unexplored
  branch of the evidence graph, so page 2 of a flow question is "the other links of these files", not "more files".
- **The repeat table.** `rr_check`/`rr_after` move to the request record. Two changes of behaviour to decide with
  the owner (they are bug fixes, section 8): a plan update (`TodoWrite`, `TaskUpdate`) no longer clears the table;
  only a tool that changes files or runs a command does.
- **The neighbour note.** `fg_neighbor_note` takes its "cited" and "read" sets (`g_fg_cited`, `g_fg_read`) from the
  request record, so neighbours already in the packet are not announced again, and prefers the unexplored ends of
  the evidence graph over generic file-graph neighbours.
- **The plan nudge.** `loop_plan_note` skips the "plan first" note when the intent is a question (`UI_LOCATE`,
  `UI_USAGE`, `UI_FLOW`, `UI_CONFIG`, `UI_HOWTO`, `UI_HISTORY`, `UI_ERROR` without `change`) and the calls so far
  are read-only: a question answered with reads is not a job to plan (the comment on `loop_plan_readonly` already
  says so; the request kind makes it reliable).
- **Hypotheses and gaps across turns.** A follow-up with no informative term ("e o outro?", "ok, e onde isso
  chega?") gets no retrieval (rule C1) **unless** the session has an open hypothesis: then the follow-up inherits the
  previous intent's entities and the pipeline continues from the open ends (P2). This is the only place the
  classifier reads state, and the trace says "inherited from request N".

State is memory, not truth: it is never persisted as fact, a resumed session starts with an empty state, and
everything reused from it is verified again.

#### 3.6.3 Result cache and invalidation

- **Key**: hash of (connector id, connector version, normalized arguments, root, **index generation** of the store for
  index connectors, or the (size, mtime) of each file touched for file connectors).
- **Where**: in memory, per session, LRU of 256 entries, at most 4 MB. No disk cache in the first version, except
  the expander's term map (`terms.tsv`, `docs/code-intelligence.md` 5.3) when that connector exists.
- **Invalidation**: a new manifest generation invalidates every index entry of that root (one integer compare);
  `index_touch(path)` from `Edit`, `Write`, `MultiEdit`, `NotebookEdit` (lifecycle phase 1) invalidates entries
  that touched the path; a `Bash` call sets "may have changed" and forces the stamps to be rechecked; `/cd` drops
  the entries of other roots lazily.
- **Never cached**: `session.facts`, `memory.recall` (the file is tiny and changes), anything with
  `UE_SIDE_EFFECTS`. Web results reuse the page memory `webnav.c` already keeps and are not cached a second time.
- A cache hit is still verified (P5): the cache saves the query, not the proof.

### 3.7 UIOE block 5: Orchestrate

Orchestrate owns the multi-step part: the order of states, the dependencies between sources, the single bounded
retry, fusion and deduplication of what several connectors returned, tie-breaking, and the stop rules.

**Stop rules**, all deterministic: the pipeline's last step ran; the deadline expired; the path is complete and
verified (no open end that a remaining step could close); the fan-out limits were reached; two consecutive steps
added no new node. Whatever stopped it is written in the trace and, when it limits the result, in `Not found /
limits`.

**Retries**: a step is retried once only when its connector returned `UR_TIMEOUT` with partial data and budget
remains, with half the remaining time. An error is never retried in the same request; the breaker counts it.

**Dependencies between sources**: a step's input may be the output of an earlier step ("the keys of the top 5
files", "the customer id the database returned"). A dependent step whose source step came back empty is skipped
and recorded as "not run: nothing to start from".

**Deduplication**: the same node from two connectors is one node (3.7.3 identity); overlapping ranges of one file
are merged; a document line that only restates a path step is attached to that step as corroboration instead of
being listed again.

**Tie-breaking**, in order: higher tier (3.7.4); verified over unverified; closer to an entity of the request (same
file, then same folder); not yet seen in this request; shorter range; path order as a last, stable resort.

#### 3.7.1 States

```
P0 UNDERSTAND  interpret, classify, route (3.3 to 3.5)              -> UioeAsk, UioeClass, UioeRoute   (rules)
                 retrieve = 0, or gate rule G1 or G2 -> END, no block, state untouched
P1 SCOPE       resolve root and prefix; connectors available?      -> per connector: ready | partial | absent  (registry `available`)
                 index_ensure(root, level, prefix, budget) for the levels the pipeline needs (lifecycle phase 1);
                 never waits beyond its budget; records what is partial
P2 RECALL      session state: what of this intent is already known (evidence graph of the session, 3.6.2)
                 everything needed is fresh and already shown -> END: nothing new to inject (a tool call gets "already shown: ...")
P3 ANCHOR      entities -> nodes (cascade); if none: UQ_WHERE search -> candidate anchors
P4 EXPAND      follow edges from the anchors, per pipeline (3.7.2); each expansion is a capability request
P5 CONFIRM     reread the lines of every node and edge going into the packet (3.6.1)
P6 GAPS        what the pipeline wanted and did not get; what was not consulted and why
P7 SELECT      rank (3.7.4), build the answer path; recompute the route confidence and apply the gate (3.5.3)
P8 EMIT        hand the graph and the route to the Context Builder (3.9); trace record; state update
```

Transitions are forward only, with two bounded exceptions: P4 may return to P3 **once** when the expansion found
no edge from any anchor (the anchors were wrong: take the next three candidates), and P5 may drop to P4 **once**
when verification removed every step of the path. Each state has a deadline (3.5.2); on expiry the machine jumps to
P6 with what it has. There is no other loop.

#### 3.7.2 Pipelines

Each pipeline is a static table of steps `{capability, input, condition, limit, on-empty}` in
`src/uioe/pipes.c`. "Anchor" means a node with `file:line`. K values are provisional.

**`UI_LOCATE`** (ceiling 500 tokens)

1. For each entity: `UQ_DEF` (symbol), path match (path), `UQ_LITERAL` (literal), `UQ_LINKS` key occurrences (key).
2. If no entity resolved: `UQ_WHERE` with the informative terms, K = 5; map each chunk to its enclosing symbol
   (`code.at`).
3. For the top 2 anchors: count of callers and the first `called by` (one line each), the neighbours not yet read.
4. Condition `gap >= 0.5` and nothing above confidence 45: emit only `Not found / limits` and `Next:`.

**`UI_USAGE`** (600)

1. `UQ_DEF` of the name through the cascade; several definitions are all shown.
2. By relation bit: `UQ_CALLERS` (K = 8), `UQ_USES` (K = 8), `UQ_CALLEES` (K = 8, in call order); with no bit, callers
   then non-call uses.
3. If the name is a file: `UQ_IMPORTS` (who imports it).
4. If there is no definition in the set: `UQ_LITERAL` for the name (macro-generated, a string dispatch), then
   `UQ_LINKS` (the name may be a text key, not a symbol). This is the step that turns "No definition of register"
   into "register is a message type: sent at ..., handled at ...".

**`UI_IMPACT`** (1200)

1. `UQ_DEF`; body range of the definition.
2. `UQ_IMPACT` depth 3, at most 25 nodes; grouped by depth; path confidence = weakest edge.
3. `UQ_LINKS` of the definition's file range: keys this symbol sends or handles, and their other ends (a change can
   break a consumer that never calls it).
4. `UQ_IMPORTS` of the file when the symbol is exported or file-level.
5. Tests: callers whose path `fd_is_test` accepts, listed apart ("tests that exercise it").
6. Limits always stated: calls through pointers, macro-generated names, dynamic dispatch, other repositories.

**`UI_FLOW`** (1500): the pipeline of the measured failure.

1. **Anchors.** Entities first. Otherwise `UQ_WHERE`, K = 8, code only (docs are handled in step 6).
2. **Key links of the anchors.** For each anchor file (top 5): `fl_q_file` -> the keys it sends and handles and
   where each goes. Keep links whose other end is in another file.
3. **Call links of the anchors.** For each anchor symbol (top 3): `UQ_CALLEES` depth 2 and `UQ_CALLERS` depth 1.
4. **Score each link for the question** (relevance, separate from its fact confidence): +direction match (the
   request says "send" and the anchor is the PRODUCE side), +terms of the request or their concept expansions
   present in the producer line, the key or the handler's enclosing symbol, +both ends already among the search
   candidates, -links through generic keys.
5. **Chain.** From the best link, follow forward up to 4 hops: the handler's enclosing symbol -> its own sends
   (`fl_q_file` with the line) and its callees. Stop at a symbol with no outgoing edge, at a repeated node, or at
   the hop limit. The chain is the Answer path.
6. **Corroboration from documents.** `docs.select` and `docs.search` for lines that name two or more files of the
   chain. Such a line raises the relevance of the chain (not the confidence of its facts) and is cited.
7. **Excerpts.** For each hop: the sending line and the handling line with 2 lines of context, through
   `file.windows`, verified in P5.
8. On empty step 2 and 3: back to P3 once; then the LOCATE output with the sentence "no connection found between
   the candidates in the indexed set".

**`UI_CONFIG`** (600)

1. Entity that is an environment variable or key: `fl_q_key` restricted to classes `env`, `config`, `cmd` -> where
   it is set or defined (PRODUCE) and where it is read (CONSUME), with the default value's line when the read site
   has one. Values of files flagged as secret are never printed (3.11.3).
2. No such entity: `UQ_WHERE` with the terms plus the config vocabulary, restricted to configuration files and
   documented tables (`.cfg`, `.toml`, `.yaml`, `.env.example`, README tables), then step 1 for the keys found.
3. `docs.select` / `docs.search` for the line that documents the option.
4. `Not found` names the config files that exist and were searched.

**`UI_ERROR`** (1500)

1. Literal: the quoted message or error text -> `UQ_LITERAL` (exact, then with numbers and paths masked).
2. If found in the project: `code.at` (enclosing symbol), the condition lines above it in the same body (the
   guard), `UQ_CALLERS` depth 2 of that symbol.
3. `session.facts`: the last failing call of this session and its output (`g_fc_fail`), files changed in this
   session (`fc_file`): an error that appeared after an edit points at the edit.
4. Working/failing pair named in the request: the asymmetry of `docs/code-intelligence.md` 6.2 (places that handle
   A and have no counterpart for B), an exact set difference over occurrences.
5. Not in the project: `memory.recall`, `history.search` (was it seen before, what solved it), `howto.recipes`;
   then `Next:` proposes `WebSearch` with the cleaned message. The network is touched only under the policy of 3.8.6.

**`UI_HOWTO`** (700)

1. `docs.select` (the documents `KATANA.md` points to), then `docs.search` (documents get the boost of `fd_qflags`).
2. `howto.recipes` and `skill.match` on the informative terms: a matching skill becomes the `Next:` line
   (`Skill {"skill": "name"}`).
3. Commands named in the documents found: `UQ_LINKS` class `cmd` for where the subcommand is implemented (one line).
4. Nothing local: `Next: WebSearch "..."`, and the packet says the project's documents do not cover it.

**`UI_HISTORY`** (400)

1. `session.facts`: this conversation first (the request log of 3.6.2: what was asked and concluded earlier).
2. `memory.recall` on the informative terms.
3. `history.search` scoped to the current project name when one is known.
4. No code connector runs. Each fact carries its date; an old fact about code gets the limit "saved on DATE; the code
   may have changed since".

**`UI_WEB`** (300)

1. `memory.recall` and `docs.search` (the answer may be local).
2. Pre-LLM, the web connector runs only under the policy of 3.8.6 (off by default); otherwise
   `Next: Search the web for "..."`.
3. When the model calls `WebSearch` or `WebFetch`: unchanged behaviour; the pages enter the evidence graph as web
   nodes so a later request in the session can cite them without fetching again.

**`UI_FINDINGS`** (non-code domains; ceiling 1000; this is the multi-source pipeline)

1. **Local first.** `memory.recall` and `session.facts` for the entities; `file.glob` and `docs.search` for
   documents whose name or text carries the named entity (a contract, a report, a spreadsheet exported as text).
2. **System of record.** For domain `data`, `finance` or `legal` with a registered connector: the named query whose
   arguments the interpreted request fills completely (name, period, amount). Arguments that could not be filled
   are a limits line, not a guess.
3. **Qualify one source with another** (`depends on`): the document clause that governs the figure (step 1's
   document, the section whose words match the question's relation: "reajuste", "prazo", "multa"), through
   `file.windows`; or the external value the clause refers to (an index, a rate) through the financial connector or,
   if the route allows, the web.
4. **Agreement and conflict.** Two sources giving the same fact become one finding with both sources. Two sources
   that disagree are **both shown**, each with its date; the newer is listed first and the conflict is stated in
   `Not found / limits`. UIOE never picks a winner between sources of record.
5. **Schema instead of answer.** When no named query covers the question: the schema lines that match the
   question's words, as `Evidence`, and `Next:` with the server's query tool.
6. Every finding carries source, locator and date. A finding with no date is `(guess)`.

**`UI_OVERVIEW`** (400): `code.tree` (when built) or the directory listing, the title lines of `README`/`KATANA.md`,
the most-imported files per top folder (`code.deps`). No search.

**Modifier `change = 1`** (the request asks to edit): the pipeline of the kind, plus the full body of the top
definition (the model must read before editing) and step 2 of `UI_IMPACT` at depth 1. Ceiling 1500.

#### 3.7.3 The evidence graph

One graph per request, merged into a bounded graph per session (3.6.2).

```c
typedef enum { UN_FILE, UN_RANGE, UN_SYMBOL, UN_KEY, UN_DOCLINE, UN_FACT, UN_EPISODE, UN_WEB, UN_FOLDER, UN_SKILL } UioeNodeKind;
typedef struct {
    uint32_t id; UioeNodeKind kind;
    char path[400]; int line0, line1;        /* onde (vazio para fato de memoria) */
    char name[100], sig[160];                /* simbolo, chave, titulo */
    uint8_t cls;                             /* classe da chave (msg, event, route, env, config, cmd) ou tipo do simbolo */
    int conf; uint8_t step;                  /* 0-100 e o passo da cascata que achou (1 exato, 2 derivado, 3 provavel) */
    int relevance;                           /* 0-100: quanto casa com o pedido (so ordena; nunca vira "confianca") */
    uint8_t ver;                             /* UV_NONE, UV_OK (relido agora), UV_STALE (arquivo mudou), UV_GONE, UV_UNREADABLE */
    uint64_t stamp_size, stamp_mtime, stamp_hash;   /* do arquivo, quando foi verificado */
    uint16_t connector; char how[170];          /* proveniencia: motor e o texto curto de como foi achado */
    uint8_t shown, read, dropped;            /* estado do pedido (3.6.2) */
} UioeNode;
typedef enum { UE_CALLS, UE_DEFINES, UE_USES, UE_IMPORTS, UE_SENDS, UE_HANDLES, UE_LINKS, UE_CONTAINS, UE_MENTIONS, UE_VIA, UE_SAME_AS } UioeEdgeKind;
typedef struct { uint32_t from, to; UioeEdgeKind kind; int line; int conf; uint8_t ver; uint16_t connector; char how[170]; } UioeEdge;
```

- **Identity.** A node is keyed by (kind, path, line0, name); two connectors returning the same symbol or range merge
  into one node that remembers both provenances. Agreement between independent connectors is recorded
  (`UE_SAME_AS` is not needed for it: the node's `how` lists both) and counts in ranking (3.7.4).
- **`UE_LINKS`** is the producer-to-consumer edge of `fl_pair`; `UE_SENDS`/`UE_HANDLES` connect a range to a key
  node. `UE_MENTIONS` connects a document line or a memory fact to the file or symbol it names. `UE_VIA` is the
  file-graph edge of `fg_evidence` (probable by nature).
- **Path confidence** = the confidence of its weakest edge (the rule `impact` already uses). The entry's relevance
  is printed beside it, never multiplied into it.
- **Size.** At most 400 nodes and 800 edges per request (fixed arrays in `UioeCtx`); on overflow the lowest
  relevance is dropped and counted in `Not found / limits`.
- **Session graph.** A ring of the nodes shown or read in the last 8 requests, at most 1500 nodes. It is an
  optimization: every node reused from it is verified again before it is shown.

#### 3.7.4 Fusion and ranking

The rule already used by the coder search (tiers first, reciprocal rank fusion inside a tier; `g_cs_rrfk = 60`)
is kept and lifted one level, from channels to connectors:

1. **Tiers by how the item entered**, not by score: (1) named in the request and resolved by the cascade's exact
   step; (2) reached from a tier-1 or tier-2 node by a verified structural edge (`calls`, `links`, `imports`);
   (3) derived step of the cascade, or corroborated by two independent connectors; (4) probable (word search only,
   `via` edges, fuzzy names). A lower tier never outranks a higher one.
2. **Inside a tier: RRF** over the rank lists of the connectors that returned the item, weight 1 per connector and 0.6
   for conceptual and history sources (the value `g_cs_chw` uses for the conceptual channel).
3. **Relevance adjustments** (bounded, additive, each with a reason in the trace): direction match for flows; not
   yet seen in this request (+) or already read (-, it goes to "already have"); document or test file when the
   question is not about documents or tests (the `fd_rank` demotion); same folder as an anchor.
4. **The answer path is not ranked, it is built** (3.7.2): the ranking only chooses its entry.
5. **Never one probable candidate with an air of certainty.** With no tier-1 or tier-2 item, at least three
   candidates are shown if three exist, and the header says the entry was chosen from words.

### 3.8 SOURCES: connectors

The owner's diagram names six kinds of source: Filesystem, Web search, Financial APIs, Code repositories, Legal
sources, Databases. In katana each source is reached through a **connector** registered behind one interface.

#### 3.8.1 In process or external (D4)

| Source of the diagram | Connectors | Where they run | State |
|---|---|---|---|
| Filesystem | `file.windows`, `file.grep`, `file.glob`, `place.resolve` | in process | the code exists as tools; adapters to write |
| Code repositories | `code.search`, `code.rank`, `code.def`, `code.refs`, `code.callers`, `code.callees`, `code.uses`, `code.outline`, `code.at`, `code.deps`, `code.impact`, `code.flow`, `code.tree` | in process (the `coder` module) | exists; needs the structured API of 3.8.4 |
| (documents, part of Filesystem and Code) | `docs.select`, `docs.search`, `howto.recipes` | in process | exists |
| (memory; not in the diagram, already in katana) | `memory.recall`, `history.search`, `session.facts` | in process | exists |
| Web search | `web.search`, `web.fetch` | in process code that calls out to the network | exists |
| Databases | `mcp:<server>` with a connector card of domain `data` | **external process, through MCP only** | missing: the card, the policy, and a server |
| Legal sources | `mcp:<server>`, domain `legal` | external, MCP only | missing |
| Financial APIs | `mcp:<server>`, domain `finance` | external, MCP only | missing |

The rule for the last three is the owner's: third-party programs stay outside katana and enter as plugins. katana
links no database driver, no HTTP client library for a paid API and no legal corpus. What katana provides is the
MCP client that exists (`src/agent/mcp.c`: `mcp_init`, `mcp_owns`, `mcp_call`, cards, the trust file, reconnection,
timeouts) and the connector interface below. UIOE works with none of them installed.

#### 3.8.2 The registry and declared capabilities

```c
/* src/uioe/types.c (assinaturas resumidas) */
typedef enum { UC_INSTANT, UC_INDEX, UC_FILES, UC_PROCESS, UC_NETWORK, UC_MODEL } UioeCost;
/* UC_INSTANT < 5 ms (memoria);  UC_INDEX < 50 ms (tabelas mapeadas);  UC_FILES < 500 ms (rele arquivos, grep);
   UC_PROCESS segundos (processo filho);  UC_NETWORK segundos, sai da maquina;  UC_MODEL geracao do modelo local */
typedef struct UioeConnector {
    const char *id;            /* "code.flow" */
    uint32_t answers;          /* bits UQ_*: o que responde (3.8.3) */
    uint32_t emits;            /* bits UN_*: tipos de no e aresta que devolve */
    UioeCost cost;
    int conf_typical;          /* 0-100: confianca tipica de um resultado deste motor (so para ordenar o plano) */
    int need_level;            /* nivel de indice exigido: -1 nenhum, 0 N0, 1 N1, 2 N2, 3 N3 */
    uint32_t flags;            /* UE_READONLY, UE_LEAVES_MACHINE, UE_SIDE_EFFECTS, UE_NEEDS_MODEL, UE_UNTRUSTED_OUTPUT */
    int default_ms;            /* prazo padrao da chamada */
    int (*available)(const UioeCtx *c, const char *root, char *why, size_t cap);   /* 1 pronto, 0 nao, 2 parcial */
    int (*run)(UioeCtx *c, const UioeCall *q, UioeResult *r);                      /* preenche nos e arestas; nunca imprime */
    const char *version;       /* entra na chave do cache */
} UioeConnector;
int uioe_register(const UioeConnector *e);      /* registry.c: tabela fixa de ate 48 motores */
```

Rules for every connector: it never prints, never reads global request state (it receives `UioeCtx`), honours
`c->deadline_ms`, returns `UR_OK`, `UR_PARTIAL`, `UR_EMPTY`, `UR_UNAVAILABLE` or `UR_TIMEOUT`, and fills
`r->limits` (a short text with what it did not look at). A connector that cannot say its limits is not registered.

**The connectors of the first version**

| Connector id | Existing code it wraps | Answers | Cost | Typical confidence | Precondition | Emits |
|---|---|---|---|---|---|---|
| `code.search` | `coder_lookup` -> `cs_search_core` (`src/coder/search.c`: channels exact, subword, bm25, trigram, literal, conceptual; tiers then RRF) | where is X, by words, names or quoted text | index | 35 to 90 by channel | N1 | file range, symbol |
| `code.rank` | `fd_rank` + `fg_activate` + `fg_evidence` (`find.c`, `findgraph.c`): chunk ranking and sparse activation | where, by words; neighbours | index | 35 | the Find cache today; N2/N3 graph after lifecycle phase 7 | file range, `via` edge |
| `code.def` `code.refs` `code.callers` `code.callees` `code.uses` | `sy_cascade`, `sy_defs_of`, `sy_refs_of`, `coder_callees_main` (`symquery.c`) | definition, uses, who calls, what it calls | index | 92 / 62 / 32 by cascade step | N2 | symbol, `defines`, `calls`, `uses` edges |
| `code.outline` `code.at` | `coder_outline_main`, `coder_at_main`, `fl_encl_k` | symbols of a file, enclosing symbol of a line | index | 92 | N2 | symbol |
| `code.deps` | `coder_deps_main`, `sy_resolve_import`, `sy_importers` | imports and importers | index | 60 to 92 | N2 | `imports` edge |
| `code.impact` | `coder_impact_main` | transitive callers | index | weakest edge | N2 | `calls` path |
| `code.flow` | `fl_key_main`, `fl_file_main`, `fl_links_main`, `fl_pair` (`src/coder/flow.c`) | who sends and who handles a text key; keys of a file; file-to-file links | index | 60 to 96 | N3 (key sections 0x207 to 0x20A) | key, `sends`, `handles`, `links` edges |
| `code.tree` | Symbols action `tree` (lifecycle J.5, not built yet) | what a folder contains | instant | 92 | N0 | folder |
| `docs.select` | `ds_select` (`src/agent/docsel.c`) | project conventions, how-to in the `.md` files `KATANA.md` points to | files | 70 | `KATANA.md` or `AGENTS.md` exists | document lines |
| `docs.search` | `code.search` restricted to documents (`fd_is_doc`), boosted as `fd_qflags` bit 1 does | install, usage, guide questions | index | 55 | N1 | document lines |
| `file.windows` | `rd_best_windows` (`src/tools/read.c`) | the lines of one file that match the terms | files | 80 (read now) | the file is readable | file range |
| `file.grep` | `ct_grep` (`src/tools/grep.c`) with a literal | literal occurrences, when there is no index or it is partial | files | 90 (read now) | none | file range |
| `file.glob` | `ct_glob` | files by name pattern | files | 90 | none | file |
| `place.resolve` | `place_note`, `ff_resolve_missing` (`src/agent/locate.c`) | where a cited folder is | files | 80 | none | folder |
| `memory.recall` | `tool_recall`, `mem_read` (`src/agent/tools.c`, `~/.katana/memoria.txt`) | saved facts and preferences | instant | 60, falling with age | file exists | memory fact |
| `history.search` | `katana knowledge search` (`src/app/knowledge.c`, notes in `~/.katana/knowledge/`) | what was done or agreed in earlier sessions | files | 45 | notes exist | history episode |
| `session.facts` | the facts notebook and last failure (`fc_file`, `g_fc_fail` in `src/agent/facts.c`), the request log of 3.6.2 | what this session already read, changed, saw fail | instant | 85 | in a session | session fact |
| `howto.recipes` | `tool_howto` (`src/agent/howto.c`) | built-in recipes | instant | 60 | none | document lines |
| `skill.match` | `sk_collect` (`src/tools/skill.c`) | a skill whose name or description matches | instant | 50 | skills exist | skill (as a `Next:` suggestion only) |
| `web.search` `web.fetch` | `tool_web_search` (`src/agent/web.c`), `ct_webfetch` | things outside the machine | network | 40 | `g_web`; pre-LLM only under the policy of 3.8.6 | web page |
| `mcp:<server>` | `mcp_call` for the read-only tools of a server that has a connector card (3.8.5; `mcpx_plan_ok` in `mcp_extra.c`) | what its card declares: databases, legal sources, financial APIs | process | by kind of item (3.8.5) | server connected and trusted, card present; `auto` policy | findings, rows, clauses, schema lines |
| `model.expand` | the query expansion of `docs/code-intelligence.md` 5.3 (not built) | code words for request words | model | none: terms count only after the index confirms them | a model is loaded; off by default | terms |
| `lsp.confirm` | `src/tools/lsp.c` | confirmation of frontier edges | process | 95 | a server is installed; off by default | verification stamp |

`code.rank` and `code.search` overlap on purpose until the unified `Find` of the lifecycle document (phase 7)
exists; after it, `code.rank` reads the same store.

#### 3.8.3 Capability bits

`UQ_WHERE`, `UQ_DEF`, `UQ_USES`, `UQ_CALLERS`, `UQ_CALLEES`, `UQ_LINKS` (connections through text),
`UQ_IMPORTS`, `UQ_IMPACT`, `UQ_CONFIG`, `UQ_LITERAL`, `UQ_STRUCTURE` (outline, tree), `UQ_DOC`, `UQ_FACT`,
`UQ_HISTORY`, `UQ_EXTERNAL`, `UQ_PLACE`, and for the general domains `UQ_FACT_OF_RECORD` (a named query),
`UQ_AGGREGATE`, `UQ_SCHEMA`, `UQ_CLAUSE` (a passage of a governing document), `UQ_QUOTE` (a dated external value).
A pipeline step names a capability, not a connector; the registry returns the available connectors that have it,
cheapest first. This is what makes a missing connector a degradation instead of an error: `UQ_LITERAL` is served by `code.search` when N1 is ready and by `file.grep` when it is not.

#### 3.8.4 A structured API for the coder queries (prerequisite)

Today every structural query is a `coder_*_main(argc, argv)` that prints, and the `Symbols` tool reads it back by
redirecting file descriptors 1 and 2 to temporary files (`sym_capture` in `symtool.c`). That is unusable for an
orchestrator: it is process-wide (the desktop protocol also writes `\x01TOOL` markers on stdout), it loses
structure, and it forbids threads. The queries already build a `SyItems` array before printing (`sy_item_new`,
`sy_emit`), so the split is mechanical:

```c
/* src/coder/symquery.c e flow.c: cada consulta em duas partes */
int sy_q_def(SyView *v, const char *name, int limit, int lines, SyItems *out, SyQ *meta);
int sy_q_refs(SyView *v, const char *name, uint32_t kindmask, int limit, SyItems *out, SyQ *meta);
int sy_q_callees(SyView *v, const char *name, int limit, SyItems *out, SyQ *meta);
int sy_q_impact(SyView *v, const char *name, int depth, int nodes, SyItems *out, SyQ *meta);
int fl_q_key(SyView *v, const char *key, int limit, SyItems *out, FlQ *meta);
int fl_q_file(SyView *v, const char *file, int line, int limit, SyItems *out, FlQ *meta);
int fl_q_links(SyView *v, const char *prefix, int limit, SyItems *out, FlQ *meta);
/* coder_def_main etc. passam a ser: sy_start + sy_q_* + sy_emit; a saida impressa nao muda um byte */
```

`SyItem` already carries path, lines, kind, name, enclosing symbol, `conf`, `ver` and `how`: it maps one to one to
an evidence node or edge. `SyView` must become reusable across calls of one request (opened once in `UioeCtx`),
which is also phase 1 of the lifecycle document ("`CsView` and `SyView` cached per root and generation").

#### 3.8.5 The connector interface for external sources

An external connector is an MCP server plus a **connector card**: a small text file that tells UIOE what the
server's tools mean. Without a card, a server's tools remain what they are today, tools the model may call; UIOE
does not route to them. Cards live in `~/.katana/connectors/<name>.conn` (user) or `.katana/connectors/` of an
initialized project, in the line format of the brief (`src/agent/brief.c`), checked by `katana uioe connector check`.

```
# sales-db: read-only access to the sales database
server: sales-db                      # the MCP server (must be in the trust file)
domain: data                          # data | legal | finance | research | files
answers: fact, aggregate, schema      # capabilities (3.8.3): what it can be asked
sensitive: yes                        # results are flagged sensitive (3.11.3)
auto: schema, named                   # what UIOE may call pre-LLM without the model asking: none | schema | named | all
sends: terms                          # what may leave the process: none | terms | question
rate: 30/min, 500/day                 # the connector's own budget; the breaker enforces it
timeout: 5s
cache: 10min                          # how long a result may be reused; 0 = never
date: updated_at                      # the field of a result row that dates it (else: the fetch time)
entities: customers.name, products.sku   # columns whose values Interpret may match as entities (a list the server returns)
query revenue_by_customer(customer, period):          # a named query: UIOE fills the arguments from the interpreted request
  tool: run_named        args: {"name": "revenue_by_customer", "customer": $customer, "from": $period.start, "to": $period.end}
  returns: rows(customer, period, total, currency, updated_at)
query schema(words):
  tool: describe         args: {"match": $words}
```

What each part means:

| Part | Rule |
|---|---|
| **Capabilities** | the same bits every connector declares (3.8.3), plus the domain. The router matches (domain, capability) to connectors |
| **Query schema** | a connector is never sent free text to interpret. UIOE calls **named queries** with typed arguments filled from the interpreted request (a name, a period, an amount), or the schema query. A question no named query covers is not answered pre-LLM: the packet gives the model the schema lines that match and `Next:` names the server's tool, so the model writes the query in one step instead of discovering the schema in five |
| **Result schema** | every result is a list of items `{text or row, source id, locator (table and key, document and clause, URL), date, confidence, how}`. A connector that cannot give a locator and a date for an item is accepted only as `(guess)` material |
| **Provenance and confidence** | the connector's typical confidence is capped by kind: a row returned by a named query from a system of record 90; a document clause located by text 80; an external API value 70 with its date; anything derived by the server's own search 50. Capped again at 40 when the item has no date |
| **Rate limits** | counted per connector in the session and, for the daily limit, in `~/.katana/connectors/<name>.count`; an exhausted limit opens the breaker and becomes a limits line ("sales-db: daily limit reached") |
| **Credentials** | never in katana. They live in the MCP server's own configuration (the `env` of its entry, as today); the card names no secret; UIOE never reads, logs or forwards a credential, and a result line that matches a secret shape is masked |
| **Cache** | by (connector, query name, arguments), for the card's duration, in memory for the session; never on disk for a `sensitive` connector |
| **Allowed without asking** (`auto`) | `none`: UIOE never calls it; the model may. `schema`: only the schema query. `named`: named queries too. `all`: any read-only tool. Default when the card is silent: `none`. Only tools the server marks read-only (`mcpx_plan_ok`) are ever called by UIOE, whatever the card says |
| **What may leave** (`sends`) | `none`: only fixed queries with no user text. `terms`: resolved entities and typed arguments. `question`: the request text. Default `terms`. The route records exactly what was sent, in the trace |

A connector with `auto: none` still helps: classification knows the domain exists, and the packet can say "this is
a question for sales-db; call `mcp__sales-db__run_named`" in `Next:`.

#### 3.8.6 Policy summary: what UIOE does without being asked

| Source | Pre-LLM, unasked | When the model calls its tool |
|---|---|---|
| Filesystem, code repository, documents, memory, earlier sessions | yes (local, read-only), within the read-access rules of the tools | yes |
| Web search | only when classification is `research` or `UI_WEB` with a strong cue, the web is enabled (`g_web`) and `KATANA_UIOE_WEB=1`; it sends the interpreted terms, not the request. Default off: the packet proposes `WebSearch` in `Next:` | yes, unchanged |
| External connectors (database, legal, financial) | only what the card's `auto` allows, read-only tools of a trusted server | yes, unchanged |
| Anything that changes state | never | the tool's own rules (`KATANA_CONFIRM`) |

### 3.9 CONTEXT BUILDER

A component of its own (`src/uioe/ctxb.c`), between UIOE and the model. UIOE hands it the evidence graph, the route
and the request state; it returns one block of text for one target model. It decides nothing about sources and
runs no query.

```c
typedef struct {
    const char *name;                   /* "qwen-chatml", "gemma4" */
    int tokens_max;                     /* teto do bloco (1500) */
    int bytes_per_token_x10;            /* 40 = 4 bytes por token (a regra do docsel); medido por modelo quando houver numero */
    int quote_cols;                     /* corte de uma citacao verbatim (110) */
    int steps_max;                      /* passos do Answer path (5) */
    uint8_t where;                      /* UW_USER_TAIL: no fim do turno do usuario */
} UioeTarget;
int ctxb_build(const UioeGraph *g, const UioeRoute *rt, const UioeAsk *ask, const UioeRequest *state,
               const UioeTarget *t, char *out, size_t cap, UioePackStats *st);
```

#### 3.9.1 The seven steps

| Step | What it does | Rule |
|---|---|---|
| B1 Select | chooses what enters: the path or findings, then supporting evidence, then links | by tier and relevance (3.7.4); never one word-rank candidate presented as a path |
| B2 Deduplicate | removes what the model already has | against the **request text** (a file, folder or name the user typed is not explained back; it is used as the location of a step); against the **request state** (ranges already shown or read: referred to as "already shown" in one line, never quoted again); against itself (overlapping ranges merged; the same fact from two sources is one line naming both) |
| B3 Order | fixed section order; inside a section, most relevant first | 3.9.2 |
| B4 Compress | makes each item one line | sentences built from fixed templates with fixed verbs (`sends`, `handles`, `calls`, `defines`, `reads`, `sets`, `imports`, `states`, `returns`); **a verbatim quote only when it is decisive** (the line that holds the key, the literal, the condition, the clause, the figure), cut at `quote_cols`. No summarization by a model: what cannot be said by a template is given as location plus one quoted line |
| B5 Budget | fits the ceiling | per source (below); trimming order of 4.6.3 |
| B6 Render | writes the block for the target | fixed labels and delimiters (4.6); plain lines, no tables, no bold, no markdown |
| B7 Record | updates the request state (what is now "shown"), writes the trace line with tokens per section | the state is what makes the facades not repeat the packet |

**Budget per source**, as shares of the block's ceiling, so one loud source cannot crowd out the rest: code and
files up to 60%; documents up to 25%; memory and earlier sessions up to 10%; web up to 20%; external connectors up
to 25%; `Not found / limits` at least 80 tokens reserved; `Next` up to 60. Unused share flows to the next source in
relevance order. The block never takes more than 25% of the free context (`c->ctx - pos`, the quantity `g_budget`
already uses for tool results); below 2000 free tokens no block is built.

#### 3.9.2 Position: where the block goes and why

- **In the user turn, after the user's text.** The request stays first, exactly as typed. The block follows, and
  the model's generation starts right after `[end of context]`.
- **Most relevant at the start, `Next` at the end.** Models attend best to the beginning and the end of a long
  input and worst to the middle. So the block opens with the understanding line and the first path step, puts the
  least critical material (`Evidence`, `Links`) in the middle, and ends with `Not found / limits` and `Next`, the
  two things that decide the model's next action. This order is fixed (D5).
- **One block, not three notes.** Today the user turn can receive a docsel note and a place note, each with its own
  opening sentence. With UIOE on they are folded into the one block (3.10.2).

#### 3.9.3 The system prompt and the prefix cache

The system prompt is built once per session (`agent_build_sys`) and its processed state is kept by the prefix
cache (`pfx_prepare`, `pfx_save` in `src/agent/prefix_cache.c`; the key is a hash of the prefix tokens). Therefore:

- **Nothing per request ever goes into the system prompt.** The block is in the user turn; the prefix is
  byte-identical from request to request and from session to session.
- **Only stable text enters the prefix**: one sentence in the tool guide saying how to read a context block (4.7).
  It is added once, when UIOE is switched on, and invalidates the prefix cache once.
- The memory tail that `agent_build_sys` puts in the prompt (`mem_tail(20)`) stays as it is; memory facts that
  matter for a request arrive through the block. Whether the tail should leave the prefix is a context-management
  question for the second pass (section 11).

#### 3.9.4 Compaction and old blocks

- The opening and closing delimiters are exact strings, so the compaction code (`agent_compact` in
  `src/agent/context.c`) can find a block and replace an **old** one (from an earlier request) by one line:
  `[katana: context of an earlier request removed]`. Old blocks are the first thing compaction drops: their line
  numbers may be stale and the request state no longer covers them.
- The block of the **current** request is never compacted away while the request runs.
- On a resumed session the blocks in the history are old text; the request state starts empty and anything reused
  is verified again.
- `context.c` was not examined for this document; how a replaced block interacts with live compaction from the KV
  cache must be checked when the hook is written.

#### 3.9.5 Citations

Every line of `Answer path`, `Findings` and `Evidence` starts with its confidence in brackets or `(guess)` and its
location (`file:lines`, `document:lines`, `table key`, `URL`, a date for dated sources). Steps are numbered. The
model is asked, by the one sentence of the tool guide and by the curriculum, to cite locations in its answer the
way it already does (`file:line`); the block's numbers give it something to refer to ("step 2"). A check of the
answer against the block (does every file the answer names appear in the block or in what was read afterwards) is
possible from the request state and belongs to the execution-feedback layer (section 11), not to the builder.

#### 3.9.6 The target model

The labels, the delimiters and the section order are the same for every model. What a target profile changes:

| | Qwen3.6-35B-A3B (today) | Gemma 4 (later; nothing was tried there, by the owner's rule) |
|---|---|---|
| Where | end of the user turn (ChatML `user` message), as `docsel` does today | end of the user turn of its format |
| Tokens | ceiling 1500, 4 bytes per token until a measured ratio exists | its own ratio and, if its context in use is smaller, a lower ceiling |
| `Next:` wording | tool name and arguments in plain words (`Read bridge/server.mjs lines 100-140`), never in call syntax: the call format (XML or JSON) is the model's and is constrained by `callgrammar.c` | the same words; its native call format is its own |
| Tool declarations | none in the prompt (`--no-tool-decl`): the block must name only tools the curriculum taught | the template requires declarations; the block is unchanged |
| Reasoning | the desktop starts with reasoning off; the block must be actionable without it | to be decided there |

### 3.10 LLM EXECUTION

The agent loop of today (`src/agent/loop.c`). Three interfaces matter.

#### 3.10.1 UIOE -> Context Builder -> loop

```c
/* src/agent/loop.c, no bloco que prepara a mensagem do usuario, no lugar das chamadas a ds_select e place_note */
if (uioe_mode() != UIOE_OFF && uioe_prepare(&g_uioe, line_original, uioe_target(m), blk, sizeof blk, &st))
    append_to_user_turn(line, blk);          /* pedido, linha em branco, bloco */
else legacy_prehooks(line);                  /* ds_select + place_note, como hoje */
```

The block becomes part of `line`, so it goes where the user turn goes: the pending buffer, the history, the
session file. One dim line on stderr and one `\x01` marker for the desktop say that a context block was built and
how large it is, the way docsel prints `[docs: N trecho(s) ...]` today. The buffers must be reviewed: `line` and
the `nl` scratch are 16384 bytes, and a 6000-byte block plus a long request fits, but a block plus today's
8000-byte docsel note would not (they are never both present when UIOE is on).

#### 3.10.2 What happens to the pre-hooks that exist

| Hook today | With `KATANA_UIOE=off` | With `pack` or `full` |
|---|---|---|
| `intent_set_user` (the text `Read` uses to pick windows) | unchanged | the request record of 3.6.2; `Read` asks UIOE for the terms |
| `ds_select` (docsel note, 600 tokens) | unchanged | becomes the `docs.select` connector; its lines arrive as `Evidence` in the block; the separate note is not appended. `KATANA_DOCSEL=0` still turns the source off |
| `place_note` (a cited folder that does not exist from the working folder) | unchanged | becomes Interpret's place resolution. The located folder appears in `Question understood as: ... (project /path)` and in `Next:` as absolute paths. When nothing else qualifies, rule G5 still emits the short form, because the place alone saves a failed step (session 1 of 1.1) |
| `autoindex_start`, `autoindex_dir` (index a folder in the background) | unchanged | unchanged at session start and `/cd`; for the roots of a request it becomes Route's `index_ensure` call with a budget (lifecycle phase 1) |
| undo note, old-todo note, plan-mode note, images (`vis_take`) | unchanged | unchanged: they are not retrieval. Order in the user turn: undo note, request, context block, todo note; the plan-mode prefix stays first |
| `rr_reset`, `rp_*`, `bk_*` (per-request counters) | unchanged | the request record; same behaviour (3.6.2 lists two proposed fixes) |

#### 3.10.3 Inside the loop: the tools as facades, and asking for more context

With `KATANA_UIOE=full`:

- **`Find`** (default mode) is the model's way to ask for more context. Same name, same arguments. It runs the
  same engine with the request state, so it returns what the packet did **not** already show: the next branch of
  the path, the other links of the same files, the next candidates. Its result uses the same labels as the block,
  without the two bracket lines (a tool result is already data). A repeated `Find` continues instead of repeating.
- **`Find mode=refs`** and every **`Symbols`** action keep their output byte for byte; they gain the shared state,
  the cache and the trace. `mode=refs` with a sentence is interpreted, not refused.
- **`Read`** of a file the block points into shows the pointed lines first; a range already delivered keeps today's
  cursor note.
- **`Grep`** with a natural-language phrase runs and appends one note pointing to `Find` (rule R1 of
  `docs/investigation-engine.md`).
- **Loop notes**: the neighbour note (`fg_neighbor_note`) skips what the block cited and prefers the open ends of
  the evidence graph; the plan nudge (`loop_plan_note`) is not sent for a question answered with reads.
- **No new tool** (D1). A `Find mode=context` that returns the full block on demand is a possible later extension
  and is not part of this design: it would add a `mode` value the curriculum must teach, for a behaviour the
  default mode already has.

With `KATANA_UIOE=pack` the tools are exactly today's; only the pre-LLM block exists. That is the safest first
switch, and it is why the facades are a separate setting.

### 3.11 Across the blocks

#### 3.11.1 Trace and observability

`~/.katana/uioe.log` (`KATANA_UIOE_LOG` overrides; `KATANA_UIOE_LOG=0` turns it off; rotated to `uioe.log.1` at
8 MB). Plain text, one record per request, readable without a tool:

```
== 2026-11-23 11:26:45 req 7f3a origin=request session=20261123-112645-122 root=/mnt/dados/source/greymod
ask     lang=pt gap=0.33 ambiguous=no   class domain=code kind=flow sure=1 rule=R70 (cue "envia", relation send) retrieve=1 (C6)
        entities: folder mnt/dados/greymod -> /mnt/dados/source/greymod (located)   dropped: projeto, pasta, sabe, dizer
        terms: katana, sessao>session, celular>phone, envia>send
scope   code.search ready(N1) code.flow ready(N3) code.callers ready(N2) docs.select ready memory.recall ready web.search not routed(policy)
route   confidence 65 before retrieval; code/flow: anchors > key-links > call-links > score > chain > docs > excerpts
step    P3 code.search "katana session phone" k=8                 3.1 ms  8 hits   (cache miss)
step    P4 code.flow file=bridge/grey-channel.mjs                 1.2 ms  5 links
step    P4 code.flow file=bridge/server.mjs                       0.9 ms  2 links
step    P4 code.callees connect depth=2                           0.6 ms  4 edges
score   L1 grey-channel.mjs:61 -> channels.mjs:33 "register" conf=86 rel=74 (direction +20, term sessionId +18, both ends candidates +12)
step    P4 chain from channels.mjs:33: sends "terminals_changed" at 37; no handler in the indexed set
step    P5 verify 6 ranges                                        2.4 ms  6 ok
gaps    no handler of "terminals_changed" in the indexed set (373 files analysed for keys); web not consulted
gate    confidence 85 after retrieval (verified link by structure +20) -> G4 packet   sent out: nothing
build   block 331 tokens: path 3 steps, evidence 0, links 2, notfound 2, next 1; deduplicated 1 (folder named in the request)   total 11.8 ms
```

(Illustrative: the layout is the specification; the numbers were not produced by a run.)

Commands (`src/uioe/cli.c`, dispatched from `main` like `katana coder`):

| Command | What it does |
|---|---|
| `katana uioe ask "<request>" [--cwd DIR] [--as find|symbols|request] [--tokens N] [--json] [--no-verify] [--raw]` | runs the whole pipeline with no model and prints the block exactly as the model would receive it, or "no block" with the gate rule that decided (`--raw`: the evidence graph before the Context Builder) |
| `katana uioe plan "<request>"` | classification and plan only: kind, rule, entities, connectors available, the steps that would run |
| `katana uioe explain [REQ-ID | last | N]` | prints the trace record of a request and **replays it**: re-runs classification and plan in deterministic mode on the current tree and marks every line that differs (a different rule, a connector now unavailable, a file that changed). It reproduces the decision; it does not claim the tree is the same |
| `katana uioe connectors` | the registry: capabilities, cost class, availability in this folder with the reason, breaker state |
| `katana uioe state` (and `/uioe` in the console) | the current request and session state: shown, read, hypotheses, gaps |
| `katana uioe connector check|list|show [NAME]` | connector cards (3.8.5): syntax, the server behind each, policy, rate counters |
| `katana uioe selfcheck` | registers every connector, runs each on a tiny generated folder and checks the result shapes (no model) |

`KATANA_UIOE_DET=1`: deterministic mode for curriculum generation and replays (no time cuts, no model expansion,
fixed order of equal scores). `KATANA_UIOE_TIME=1`: stage times on stderr, like `KATANA_FIND_TIME`. The action log
(`agente.log`, `agent_log`) is unchanged; each of its lines gains nothing. The two logs are joined by time and
session id, and `katana agentlog` can later count, per request, model steps against packet size (section 11,
evaluation).

#### 3.11.2 Concurrency

**Decision: the first version runs in-process connectors one after another, and starts slow connectors as processes in
parallel.** The in-process connectors are index lookups of 1 to 7 ms (the measured `Find` query time in
`docs/tools.md`); ten of them in sequence fit the 400 ms budget several times over. Threads would buy little and
the code is not ready for them. The slow connectors (web, MCP, a crawler) are already separate processes with pipes:
`tool_web_search_start` launches its sources in parallel and `tool_web_search_collect` gathers them. UIOE uses the
same pattern: start, continue with the fast steps, collect at the deadline.

Global state that blocks threads today, read in the code (to remove before any connector runs on a worker thread):

| Where | State | Problem |
|---|---|---|
| `src/coder/symtool.c` `sym_capture` | `dup2` of file descriptors 1 and 2 | process-wide; any other writer to stdout during a query is captured or lost. Removed by 3.8.4 |
| `src/tools/find.c` | `g_fd` (one `FdIndex`, re-rooted by each call), `g_fd_cnt`, `g_fd_verbose`, `g_fd_lq`/`g_fd_lpre`/`g_fd_lastq` | one index at a time; two roots in one request thrash it |
| `src/tools/findgraph.c` | `g_fg`, `g_fg_read`, `g_fg_cited`, `g_fg_notes` | same |
| `src/tools/common.c` | `g_intent_user`, `g_intent_find`, `g_rc`, `g_rs`, `g_ct_seen`, `g_launch_dir` | request state as globals (moves to `UioeRequest`) |
| `src/coder/search.c` | `g_csix`, `g_cs_sortsc` (comparator reads a global), tunables `g_cs_k1`, `g_cs_chw`, `g_cs_sum` | not reentrant |
| `src/coder/symquery.c` | `g_symods` ("one query per process") | import-resolution map shared by `deps` and `impact` |
| `src/coder/symindex.c`, `parse.c`, `flow.c`, `text.c`, `store.c` | `g_sy_m`, `g_ky_m`, `g_cp_sortsym`, `g_fl_brief`, `g_ct_kbbuf`/`g_ct_inited`, `g_store_nmerge` | indexing and formatting state (lifecycle phase 5 audits the indexing side) |
| `src/agent/loop.c` | `rr_*`, `rp_*`, `bk_*`, the statics inside `loop_plan_note` | request state as file statics |
| `src/agent/locate.c` | `g_place_dirs` | request state |
| `src/agent/tools.c` `tool_recall` | `static char *ln[4096]; static int sc[4096]` | not reentrant |
| everywhere | `getcwd`/`chdir` (the root is derived from the working directory; `Bash` moves it), `getenv` on every call | process-wide; UIOE resolves the root once in P1 and passes absolute paths |

Rule for new code: **no file-scope mutable state in `src/uioe/`**; everything hangs from `UioeCtx` (per call) or
`UioeSession` (owned by the loop, passed in). When the lifecycle's phase 5 has removed the indexing statics and
3.8.4 has removed the capture, the per-anchor expansions of P4 can run on the thread pool of `src/engine/base.c`,
one task per anchor over read-only mapped tables, with the pool idle whenever the model computes. That is an
optional later phase, not a requirement.

Between processes nothing new is needed: the store has one writer and lock-free readers
(`docs/code-index-lifecycle.md` G); UIOE only reads.

#### 3.11.3 Security

- **Content is data, and the bracket is reserved.** The system prompt tells the model that a line in
  `[katana: ...]` comes from the program and must be followed. Therefore: retrieved text appears only inside a
  labelled line of the block, as a short quote, never as a line of its own; any quoted file, document, memory, web or
  connector text that contains `[katana` or `[end of context]` is written with a space after the bracket; the special markers that `defang` (`src/agent/dispatch.c`) neutralizes in
  tool results (`<|`, `tool_call`, `tool_response`) are neutralized in packet text too. `defang` does not cover the
  bracket today; the same escape should be applied to every tool result (decision 5 of 8.2).
- **Secrets are never returned.** Files the index flags as secret (lifecycle N1 "secret detection"), `.env` files,
  key and certificate files, and lines matching the token shapes that `kn_mask` (`src/app/knowledge.c`) already
  masks: UIOE prints the key name and the location, never the value. `code.flow` class `env` shows who sets and who
  reads a variable, not what it is set to. Memory facts pass the same mask.
- **Nothing leaves the machine unless the route decided it** (D6). A connector flagged `UE_LEAVES_MACHINE` (the web)
  or reached through an external process (MCP) runs pre-LLM only under its policy (3.8.5, 3.8.6), receives the
  interpreted terms and typed arguments, never the raw request by default, and what was sent is written in the
  trace. No connector with side effects is ever called by UIOE.
- **Read access follows the tools.** Every path goes through the same checks the tools use (`agent_read_access`,
  the brief's `never:` patterns in `brief_guard`). UIOE cannot show a range `Read` would refuse.
- **External connectors**: only servers already trusted (`mcp-trust`), only tools the server marks read-only, only
  what the card's `auto` allows; results are untrusted data, capped in confidence by kind and masked for secrets.
  Credentials stay in the server's configuration and never pass through UIOE.
- **Limits**: the block's byte ceiling, quotes cut at 110 characters, at most 5 path steps and 6 evidence lines, file
  reads capped by the
  verification budget; a path outside the resolved roots is never opened by a pipeline.
- **The trace holds request text.** `uioe.log` is created with mode 0600, like the session files, and masked with
  the same token shapes.
- `KATANA_CONFIRM=1` is unaffected: UIOE performs no action that asks for confirmation.

#### 3.11.4 Extensibility

- **A new in-binary connector** = one `UioeConnector` row (capabilities, cost, precondition, flags) + one `run` function
  that fills nodes and edges + one line in the registration function. No pipeline changes when the connector serves an
  existing capability; a new capability bit needs one step in the pipelines that want it.
- **A new intent** = one cue table, one rule number, one pipeline table, one ceiling, one curriculum family.
- **Third-party programs stay outside** (owner's rule: third parties enter as plugins). An external connector is a
  separate process reached through MCP: an `mcp:<server>` connector with its card (3.8.5) puts a trusted server's read-only tools in the registry. No third-party library is linked, and UIOE works with no
  plugin installed. A language server is the same case through `lsp.confirm`.
- **A future module of another kind** (documents, data: the reason the code module is called "coder") registers
  its connectors the same way. That is what "unified" means here: the registry, planner, evidence graph and packet are
  not specific to code; only the connectors and the pipelines' steps are.

## 4. Contract and formats

Four objects cross the boundaries of the diagram: the interpreted request (Interpret and Classify -> Route), the
route plan (Route -> Retrieve and Orchestrate), the connector result (Sources -> UIOE) and the context packet
(Context Builder -> model). Only the last one is seen by the model; the first three are seen by the owner, in the
trace and in `katana uioe plan`.

### 4.1 The constraint: the model learns formats from training

The model is trained with `--no-tool-decl`: the tools section is left out of the prompt, and names, arguments and
output shapes come from the curriculum (`data/curriculum/v7/README.md`). Curriculum v7 was generated on 2026-11-23
and **has not been trained yet**; lifecycle phase 9 already requires a v8. So a visible change is cheap now if it
is **frozen before v8 is generated**, and expensive after a model has been trained on a format.

### 4.2 The pattern chosen (D1): pre-LLM first, facades after, no new tool

| | Pre-LLM block in the user turn | `Find` and `Symbols` as facades | A new tool (`Investigate`) |
|---|---|---|---|
| Role | **the main case** | the follow-up inside the loop | not built |
| Model steps saved | the first search steps too; best case the model answers in its first step | the steps after the call | same as a facade |
| Prompt cost | one stable sentence; the block's tokens on qualifying requests only | none | one more name to learn, competing with `Find` |
| Main risk | a wrong block sends a small model the wrong way | `Find`'s output changes shape | the model chooses badly among similar tools |
| Protection | the gate of 3.5.3; off by default | a separate setting (`full`) | - |
| Training (v8) | a family that reads the block and continues from `Next` | the Find families regenerated | a new family plus arbitration against `Find` |

Decision: when a request arrives, UIOE runs with the pre-hooks that exist (and replaces them, 3.10.2) and injects
**one** block in the **user turn**. The system prompt does not change per request. Inside the loop the model keeps
using `Find`, `Symbols` and `Read` as follow-up, and they are facades of the same engine with shared request
state. No `Investigate` tool in the initial phases; `Find mode=context` is only a possible later extension (3.10.3).

One adjustment to the delegated pattern, from the code: the block is appended **after** the user's text, not
before it, because that is where `ds_select` and `place_note` already put their notes, where the curriculum
families `x-place` and `x-docs` expect engine text, and where `Next` ends up last before generation.

### 4.3 The interpreted request

Structures `UioeAsk` (3.3) and `UioeClass` (3.4). Text form, as `katana uioe plan` and the trace print it:

```
ask       "eu tenho um projeto chamado greymod que esta na pasta mnt/dados/greymod ... como o katana envia as informacoes da sessao para o celular?"
lang      pt
entities  folder "mnt/dados/greymod" -> /mnt/dados/source/greymod (located by name, 80)
          name "greymod" (project; same folder)      concept sessao>session   concept celular>phone   concept envia>send
dropped   projeto, pasta, sabe, dizer, tenho
relation  send/receive (direction: send)
gap       0.33      ambiguous no
class     domain code (rule D-resolved: the folder is a project with a code index)   kind flow (R70, sure)   complexity multi-hop
          retrieve 1 (C6)   risk: none (read-only)
```

### 4.4 The route plan

Structure `UioeRoute` (3.5). Text form:

```
route     pipeline code/flow   deadline 400 ms   ceiling 1500 tokens   confidence before retrieval 65
  1  where     code.search   "session send phone katana" k=8, code only            local
  2  links     code.flow     file=<each of the top 5 of step 1>                    local   depends on 1
  3  callees   code.callees  <symbols of the top 3 of step 1> depth 2              local   depends on 1
  4  chain     (orchestrate) follow the best link up to 4 hops                     local   depends on 2, 3
  5  doc       docs.select, docs.search  lines naming two files of the chain       local   depends on 4
  6  verify    reread every line that will be shown                                local
  not routed: web (not needed: the question is about a local project), memory (no memory cue), mcp (no connector for domain code)
  sends out: nothing
decision  after retrieval: confidence 85 (verified link by structure +20) -> G4 packet
```

### 4.5 The connector result

What every connector returns, in process or external. It is the only thing a connector may produce.

```c
typedef struct {
    int status;                         /* UR_OK, UR_PARTIAL, UR_EMPTY, UR_UNAVAILABLE, UR_TIMEOUT, UR_DENIED (politica), UR_LIMIT (taxa) */
    UioeNode *node; int nnode;          /* itens: onde (arquivo:linhas, tabela e chave, documento e clausula, URL), o que, confianca, como */
    UioeEdge *edge; int nedge;          /* ligacoes entre itens, quando a fonte as conhece */
    char source[64];                    /* id do conector e, se externo, do servidor */
    int64_t as_of;                      /* data do dado (do campo `date` da ficha) ou a hora da consulta */
    char limits[300];                   /* o que este conector NAO olhou: escopo, nivel do indice, pastas ignoradas, linhas cortadas */
    char sent[200];                     /* o que saiu da maquina nesta chamada ("" = nada) */
    double ms; uint8_t from_cache, sensitive;
} UioeResult;
```

Rules: a result with no `limits` text is rejected by the registry's self-check; an item with no locator can only be
`(guess)`; `sent` is copied to the trace verbatim; a `sensitive` result is never cached on disk and its items are
masked by the secret shapes before they reach the builder.

### 4.6 The context packet (normative)

The format below is normative. It was decided from the point of view of the reader: a model that receives the
block cold, with a small context, reasoning off, and one to two minutes of cost for every wrong turn.

#### 4.6.1 The ten rules

1. **Open with the understanding of the request in one line** (`Question understood as: ...`), so the model can
   check that the program understood before it trusts anything else.
2. **An answer path, not a pile of search results**: 2 to 5 numbered steps, each with an exact location
   (`file:lines`), a minimal verbatim quote only when decisive, and a confidence 0 to 100.
3. **A fixed vocabulary of labels**, identical in every packet: `Question understood as`, `Answer path` (or
   `Findings`), `Evidence`, `Links`, `Not found / limits`, `Next`.
4. **Plain text in lines.** No tables, no bold, no heavy markdown. Stable delimiters: `[katana: context ...]` and
   `[end of context]`.
5. **Verified fact apart from guess.** An item checked by rereading the source carries its number in brackets; an
   unchecked item carries `(guess)`.
6. **`Not found / limits` is always present and specific, with the reason** (outside the indexed set, another
   project, an ignored folder, a partial index, a source not installed), so the model does not repeat the search.
7. **`Next:` in the imperative, with a stop criterion** ("Read X lines a-b, then answer. Do not search again for
   what is listed above.").
8. **Short**: at most 1500 tokens, most relevant first.
9. **No repetition** of what the user's request already contains or of what the request state already showed.
10. **The same skeleton for every domain**: outside code, `Answer path` becomes `Findings`, and each line cites
    its source and date.

#### 4.6.2 The format, on the measured question

```
[katana: context for this request. Gathered by the program before you started. Items marked with a number were checked in the files; (guess) means not checked.]
Question understood as: explain how katana sends its session information to the phone (project /mnt/dados/source/greymod).
Answer path:
1. [95] bridge/grey-channel.mjs:55-61 connect() opens the websocket /channel and sends {type:"register", cwd, project, sessionId, agent, pid}.
2. [92] bridge/channels.mjs:22 accepts only the path /channel; :33 handles type "register" (onLink, 29-78) and :37 broadcasts "terminals_changed".
3. [70] bridge/server.mjs:111-117 sessionsForPhone() builds the session list for the phone, including linked terminals without a transcript (katana).
Links: "register" grey-channel.mjs:61 -> channels.mjs:33 [79]; "/channel" grey-channel.mjs:58 -> channels.mjs:22 [83].
Not found / limits: nobody sets KATANA_SESSION_ID inside this project (katana sets it in /mnt/dados/source/katana/src/agent/mcp.c, another project, not indexed here); the phone app (Kotlin) side was not traced.
Next: Read bridge/server.mjs lines 105-120, then answer. Do not search again for what is listed above.
[end of context]
```

The locations in this example exist in the tree as written (lines 55 to 61, 58, 22, 33, 37 and 111 to 117 were
read for this document; `KATANA_SESSION_ID` is set in `src/agent/mcp.c`). The confidence numbers are illustrative:
nothing was run. Against the example the owner approved, two refinements: step 3 points at the function's own
range (111-117) instead of a 40-line window, because a narrower location is a cheaper `Read`; and step 2 adds the
broadcast at line 37, the hop that connects step 2 to step 3.

#### 4.6.3 Grammar

| Part | Form | Rules |
|---|---|---|
| Opening | `[katana: context for this request. Gathered by the program before you started. Items marked with a number were checked in the files; (guess) means not checked.]` | fixed text, one line. For non-file sources the words "in the files" become "at the source" |
| `Question understood as:` | one sentence, in English, restating the request with the resolved place in parentheses | built from the task kind's template and the resolved entities; never the user's words copied back |
| `Answer path:` | `N. [C] location text.` one step per line, 2 to 5 steps | a step joins the previous one by a verified connection; the text uses the fixed verbs; a quote only when decisive, inside the sentence; a step that could not be reread is `N. (guess) ...` and is never step 1 |
| `Findings:` | same line form | replaces `Answer path` when the items are facts from sources, not a chain; each line ends with its source and date in parentheses |
| `Evidence:` | `- [C] location text.` | optional; supporting items that are not steps: a document line, a memory fact (with its date), a definition the path uses, a schema line. At most 6 |
| `Links:` | `"key" A:line -> B:line [C]` or `caller() A:line calls callee() B:line [C]`; several on one line separated by `; ` when two, one per line when more | optional; connections not already steps. A dangling end is written `-> nobody handles it in the indexed set` |
| `Not found / limits:` | one or more clauses separated by `; `, or `- ` lines when more than three | mandatory. Each clause is **what** was not found or not looked at and **why**: `not in the indexed set (N files)`, `in another project, not indexed here`, `in an ignored folder (name)`, `index partial: symbols 3100 of 98000 files`, `no connector installed for this`, `not consulted: reason`, `left out for space: location` |
| `Next:` | one imperative sentence with a concrete action and a stop criterion; at most two actions | forms: `Read FILE lines a-b, then answer.` / `Answer from the path above.` / `Call Symbols flow for "key", then answer.` / `Ask the user which of A or B they mean.` / `Search the web for "terms".` It ends with `Do not search again for what is listed above.` when the block has an `Answer path` or `Findings` |
| Closing | `[end of context]` | exact string, alone on its line |

Confidence is written as the number alone (`[95]`); the level word (`high`, `medium`, `low`) stays in the outputs
of `Symbols` and `Find mode=refs`, where it exists today. Inside the block a number costs fewer tokens and the
`(guess)` mark carries the distinction that matters.

**Shares and trimming.** Opening, understanding line and closing about 90 tokens; path or findings up to 45%;
evidence up to 20%; links up to 10%; `Not found / limits` at least 80 reserved; `Next` up to 60. When over the
ceiling, in order: evidence lines from the bottom (each named in "left out for space" with its location); links
beyond the first three; quotes inside steps 3 to 5; never a path step, never a `Not found / limits` clause, never
a confidence mark.

**Escaping.** Any retrieved text that contains `[katana` or `[end of context]` is written with a space after the
bracket; the chat-format markers that `defang` (`src/agent/dispatch.c`) neutralizes in tool results are
neutralized here too. Quotes are cut at 110 characters and never span lines.

#### 4.6.4 When retrieval is weak

| Situation | What is injected |
|---|---|
| Route confidence below the gate and nothing authoritative to say (G1, G2, G6) | **nothing**. The request reaches the model exactly as typed |
| The request is ambiguous (G3) | nothing, or the limits form with the two readings: `Not found / limits: "sessions" matches two unrelated things here: bridge/sessions.mjs (transcript files) and the launcher's PcSessions.kt (the phone list). Next: Ask the user which one they mean, or Find with the name.` |
| A named thing does not exist in a complete scope (G5) | the limits form: understanding line, `Not found / limits: no symbol "sendSessionToPhone" in the indexed set (373 files, complete); similar names: sessionsForPhone, listSessions. Next: Symbols def for sessionsForPhone, or tell the user the name does not exist.` |
| A cited place was located and nothing else qualifies (G5) | the limits form: `Question understood as: ... (project /mnt/dados/source/greymod; the folder you named does not exist from the working folder). Not found / limits: nothing was searched yet. Next: use path=/mnt/dados/source/greymod in Find, Glob and Grep.` |
| The index is partial or absent (G5) | the limits form with the index state and what to use meanwhile |
| A path exists but its first step is a guess | no `Answer path`: the candidates go under `Evidence` as `(guess)` lines, at least three, and `Next:` names one concrete check. Only in the tool-result form (the model asked); pre-LLM this case injects nothing |

The limits form has three labels (`Question understood as`, `Not found / limits`, `Next`) between the same
delimiters. It is at most about 120 tokens.

#### 4.6.5 The same skeleton in another domain

Hypothetical connectors (`sales-db` and a contracts folder); nothing here exists. It shows the shape only.

```
[katana: context for this request. Gathered by the program before you started. Items marked with a number were checked at the source; (guess) means not checked.]
Question understood as: total revenue from customer Acme in 2026-07-01 to 2026-09-30, and whether the contract allows a price adjustment in 2026.
Findings:
1. [90] sales-db revenue_by_customer(Acme, 2026-Q3): total 184,300.00 BRL, 3 invoices (database row, updated 2026-10-02).
2. [80] contracts/acme-2024.md:88-93 clause 7.2 states "os precos serao reajustados a cada 12 meses pelo IPCA acumulado" (file dated 2024-03-11).
3. [80] contracts/acme-2024.md:12 the contract is dated 2024-03-11, so the 12-month anniversary falls in March of each year.
Evidence:
- (guess) memory 2026-05-20: "reajuste da Acme ficou para depois da renovacao" (a saved note; not checked against the contract).
Not found / limits: the IPCA figure for the period was not fetched (web not consulted: sending terms outside was not enabled); no amendment to the contract was found in contracts/ (14 files searched); sales-db has no table of price adjustments.
Next: Answer from the findings above, saying the index value is still needed. Do not search again for what is listed above.
[end of context]
```

#### 4.6.6 As a tool result (the facade form)

When the model calls `Find` (with `KATANA_UIOE=full`) the result uses the same labels, in the same order, without
the opening and closing lines (a tool result is already data and already delimited by the chat format), and
without the understanding line when the query is an identifier. `Symbols` and `Find mode=refs` keep their own
formats.

### 4.7 The one sentence in the system prompt

Added to the tool guide of `agent_build_sys` (`src/agent/prompt.c`), next to the `Find` sentence, stable for every
request: "A request may end with a `[katana: context ...]` block made by the program: check `Question understood
as`, use `Answer path` or `Findings` as your starting point, treat `(guess)` items as places to look, do not search
again for what `Not found / limits` lists, and do what `Next` says before anything else." In the same edit the
`Symbols` action list of that guide gains `flow`, which the tool has and the sentence omits today.

### 4.8 Switches (D8)

| Variable | Default | Meaning |
|---|---|---|
| `KATANA_UIOE` | `off` | `off`: today's behaviour, byte for byte. `pack`: the pre-LLM block (the pre-hooks folded into it); tools unchanged. `full`: the block plus `Find`, `Read`, `Grep` and the loop notes as facades of the same engine |
| `KATANA_UIOE_GATE` | `70` | the route-confidence threshold of rule G4 |
| `KATANA_UIOE_TOKENS` | `1500` | ceiling of the block (hard maximum 3000) |
| `KATANA_UIOE_MS`, `KATANA_UIOE_TOOL_MS` | `400`, `1500` | deadlines: pre-LLM, and when a tool asked |
| `KATANA_UIOE_WEB` | `0` | `1`: the route may send interpreted terms to the web connector pre-LLM (3.8.6) |
| `KATANA_UIOE_EXPAND` | `0` | `1`: the local model may propose terms for low-confidence requests |
| `KATANA_UIOE_LOG` | `~/.katana/uioe.log` | the trace; `0` turns it off |
| `KATANA_UIOE_DET` | `0` | deterministic mode for curriculum generation and replays |

The default stays `off` until the owner has tried `pack` on the desktop and says to turn it on (the owner's rule:
a risky, unmeasured feature is born switched off).

### 4.9 What curriculum v8 must teach (D10)

The model must meet the block in training before it meets it in use. Exactly what changes in what the model sees,
when UIOE is on:

| # | Change of format | Where it appears | v7 families affected |
|---|---|---|---|
| 1 | A user turn may end with `[katana: context ...]` ... `[end of context]` | the request | new family (below) |
| 2 | The docsel note `[katana: the engine picked this from the project documents ...]` no longer appears; document lines arrive as `Evidence` | the request | `x-docs` regenerated |
| 3 | The place note `[katana: you mention "X", which does not exist ...]` no longer appears; the place is in `Question understood as` and `Next` | the request | `x-place` regenerated |
| 4 | One new sentence in the tool guide; `flow` in the `Symbols` action list | the system prompt of every example (as v7 did for its wording change) | all (the `real` mode examples) |
| 5 | `Find` default mode answers with the labels of the block; its last line is `Next: ...` instead of `Follow a symbol with Find mode=refs, or Read a part with offset and limit.` (`full` only) | tool result | `x-find7`, `x-find6`, `x-batch`, `x-concept`, `x-investigate`, `x-explain` regenerated |
| 6 | A repeated `Find` continues from the next branch (`full` only) | tool result | `x-katananotes` (the Find repeat cell) |
| 7 | If decision 4 of section 8 is accepted: a plan update no longer clears the repeat table | loop note | `x-katananotes`, `x-cursor` |
| 8 | Unchanged: tool names and arguments; `Symbols` and `Find mode=refs` outputs; `Read`, `Glob`; the `[katana: ...]` loop notes | - | - |

New families (names provisional):

| Family | Teaches | Situations |
|---|---|---|
| `x-context` | reading the block and continuing from `Next` | answer directly from a complete path; `Read` the one range `Next` names, then answer; a `(guess)` line checked before use; the block's understanding is wrong and the model says so and searches itself |
| `x-context-limits` | acting on `Not found / limits` | the limits form: a name that does not exist (answer that, do not search); a located place (use the path from the first call); a partial index (use `Grep`); an ambiguous request (ask the user with the two options) |
| `x-context-follow` | follow-up inside the loop | `Find` after a block returns what the block did not show; no repeated search for a listed absence; `Symbols flow` on a key the block named |
| `x-context-none` | requests that get no block | chat, a command to run, "ok, continue": the model works exactly as in v7 |
| `x-findings` | the other domains | a `Findings` block with sources and dates; a conflict between two sources reported as a conflict; a missing connector named in the limits |

Generation follows the existing rule: every block is produced by the real engine in deterministic mode
(`KATANA_UIOE_DET=1`) and `katana curriculum check` replays each conversation byte for byte. v8 is generated only
after the format of 4.6 is frozen, and together with the lifecycle's own v8 requirements (the `tree` action, the
index status lines, the two `NOT FOUND` forms).

## 5. End-to-end cases

"Before" for the first case is observed. Every other number is an **estimate**: nothing was run. A model step is
taken as 1 to 2.5 minutes (the observed range between calls).

### 5.1 Code: "how does katana send the session information to the phone?" (greymod)

| | Before (observed, session `112645-122`) | After, `KATANA_UIOE=pack` (estimate) |
|---|---|---|
| t0 | the request; `place_note` appends its note | Interpret: folder located, concepts session, phone, send; Classify: code, flow (R70), multi-hop, retrieve; Route: the plan of 4.4; Retrieve and Orchestrate: search, key links of the top 5 files, chain, document corroboration, 6 ranges reread; route confidence 85 -> G4. About 15 ms of C (estimate) |
| what the model receives | the request plus a one-line place note | the request plus the block of 4.6.2 (about 330 tokens) |
| step 1 | a sentence answering the note | `Read bridge/server.mjs lines 105-120` (the `Next` line), or the answer |
| step 2 | `Find` -> six unconnected candidates | the answer, citing lines 61, 33-37, 111-117 |
| steps 3 to 18 | a whole-file read, `mode=refs` with a sentence (twice), five reads of the same file, a plan made of reads | - |
| Total | 18 calls, 26.5 min, correct (the session before it: 15 calls, 21 min, partly wrong) | 1 to 2 steps, about 2 to 5 min |

What can go wrong (honest): the request names no identifier. The pipeline reaches `grey-channel.mjs` only because
the word search ranks it among the top 5 files (it was rank 4 in the real `Find` output) and because the line that
sends the message carries `sessionId`. If the search had not ranked it, no key link would have been examined, the
route confidence would have stayed below the gate, and **nothing would have been injected**: the session would run
as it does today, no better and no worse.

### 5.2 Not code: a question that crosses local documents, a database and the web

Hypothetical: no database connector exists today. Request: "quanto faturamos com a Acme no ultimo trimestre, e o
contrato permite reajuste este ano?" in a folder that has `contracts/` and with a `sales-db` connector installed
(the card of 3.8.5).

| | Before (estimate; today the model would have only raw tools) | After (estimate) |
|---|---|---|
| Interpret | - | name "Acme" (guess by shape, confirmed by the connector's `customers.name` list and by a file name); period "ultimo trimestre" -> 2026-07-01 to 2026-09-30; "este ano" -> 2026; relation "permite reajuste" |
| Classify | - | domains data + legal(files) + finance; kind findings; complexity multi-source; retrieve; risk: sensitive |
| Route | - | (1) `file.glob`/`docs.search` for the contract; (2) `sales-db` named query `revenue_by_customer(Acme, Q3)` (card: `auto: named`, `sends: terms`: the name and the dates leave the process, to a local server); (3) `file.windows` on the contract for "reajuste" (depends on 1); (4) the index value: web, **not routed** (`KATANA_UIOE_WEB=0`) |
| Steps of the model | `Glob` for contracts, `Read` the contract (map), `Grep` reajuste, `Read` the clause, `ToolSearch` for the database tool, list tables, describe a table, write and run a query (often twice), `WebSearch` for the index: 8 to 12 steps, about 12 to 25 min, with a real chance of a wrong query | the block of 4.6.5; step 1: the answer, saying the index value is missing, or one `WebSearch` then the answer: 1 to 2 steps |
| What the block cannot do | - | decide whether the adjustment *should* be applied: that is analysis, the model's job. If the question had needed a query no named query covers, the block would carry the matching schema lines and the model would write the query in one step |

### 5.3 "Who calls `rr_check`, and what breaks if I change it?" (katana)

| | Before (estimate) | After (estimate) |
|---|---|---|
| Kind | - | impact (R50): symbol entity `rr_check` resolved at the exact step, 1 definition (`src/agent/loop.c`), cue "what breaks" |
| Steps | `ToolSearch` for Symbols (it is not in the core list), `Symbols callers`, `Read` the call site, `Symbols impact`, answer: 4 to 5 steps | pre-LLM block (a resolved entity gives a high route confidence): definition range, the call site in the loop with 3 lines, impact by depth, the paired `rr_after` and `rr_reset` (same prefix, same table: `Links`), limits ("called once; calls through pointers are not seen"). 1 step: answer; 2 if it reads the body |
| Path | - | `Answer path` = definition -> the single caller -> that caller's callers (depth 2) |

### 5.4 "Where is the configuration of the docs budget?" / "onde esta a configuracao de X"

| | Before (estimate) | After (estimate) |
|---|---|---|
| Kind | - | config (R60); entity found by the config vocabulary plus the word search -> key `KATANA_DOCSEL_TOKENS` |
| Pipeline | `Find`, `Grep` for the name, `Read` the function, maybe `Read` the README: 3 to 5 steps | `code.flow` class `env`: read at `src/agent/docsel.c` in `ds_select` with the default (600 tokens, 4 bytes each) on the same line; set nowhere in the tree; `docs.search`: the README table row. 1 to 2 steps |
| Limits | - | "no file in this folder sets it; it comes from the environment of whoever starts katana" |

### 5.5 "Why this error: `error: unknown mode "refs"`"

| | Before (estimate) | After (estimate) |
|---|---|---|
| Kind | - | error (R40): quoted literal with an error shape |
| Pipeline | `Find` with the message, `Read` around it, `Find mode=refs` on the function, more reads: 4 to 8 steps | literal found at `src/tools/find.c` in `ct_findtool`; the condition above it (the guard: `mode` set, not `refs`, not a phrase, not `neighbors`, not `search`); `session.facts`: the failing call of this session and its arguments; callers of `ct_findtool` (the tool table). 2 steps: read the guard lines, answer |
| If the message is not in the project | several fruitless searches | `Not found`: "this text is not in the indexed set (N files)"; memory and earlier sessions consulted; `Next: WebSearch "..."` |

### 5.6 "How do I install / use Y?" (documents)

| | Before (estimate) | After (estimate) |
|---|---|---|
| Kind | - | how-to (R90) |
| Pipeline | `Find` (documents are demoted unless the query has a doc word), `Read` README: 2 to 4 steps | `docs.select` and `docs.search` (documents boosted), a matching skill, the subcommand's implementation line. 1 step when the document section fits the packet; `Next: Skill name` when a skill covers it |
| Limits | - | "the project's documents do not mention Y" -> `Next: WebSearch` |

### 5.7 "What did we agree about Z?" (memory)

| | Before (estimate) | After (estimate) |
|---|---|---|
| Kind | - | history (R30) |
| Pipeline | the prompt says "Find first": `Find` (no match, its text suggests `recall`), `recall`: 2 to 3 steps, and earlier sessions are never consulted | `session.facts`, `memory.recall`, `history.search`; no code search. 1 step |
| Limits | - | each fact with its date; "nothing saved about Z" is stated, and then `Next:` offers a code search |

### 5.8 A request in another language (not Portuguese or English)

| | Before (estimate) | After (estimate) |
|---|---|---|
| Kind | - | `lang = "?"`: the cue lists do not fire. Entities still do: an identifier, a path, quoted text, a URL work in any language. With an entity: the same result as in English. Without: R99 (locate, `sure = 0`), `gap` near 1 |
| Pipeline | the model usually translates the query itself before calling `Find` (it did so in the three sessions of 1.1): no change | with `full`, the facade receives the model's translated query and interprets that; no block is injected (the route confidence is low). Same step count as today, plus a correct `Not found` |
| With the optional expander | - | one short constrained generation (estimate 2 to 5 s) proposes code terms; those that exist in the index become probable anchors. Unmeasured |

Honest summary: UIOE helps most when the answer is a **connection** (5.1, 5.3), when the route is **not code**
(5.2, 5.6, 5.7) and when the program can say **not found** with authority (5.4, 5.5). It helps little when the
request names nothing and the vocabulary does not match (5.8 without entities); there it injects nothing.

## 6. Implementation phases

Scope reminder: these phases build the five UIOE blocks, the connector interface, the Context Builder and the
loop interface. The other layers of the complete UIOE (section 11) are inventoried, not planned; their phases come
from the second pass.

Effort is in working days of one Sonnet agent with this document: an **estimate**. Every phase compiles with the
project's build and is reported as "done, compiled, nothing measured". "Done when" is observable behaviour checked
by running the commands once. Source edits follow the project rule (Python replacement scripts with count asserts).
New files go in `src/uioe/`, each included in `src/katana.c` (one line per file, after `coder/coder.c`). Everything
stays behind `KATANA_UIOE=off` until the owner turns it on.

The order is D7: (1) the core plus the code and files domains, pre-LLM, unifying `place_note`, `docsel`,
`autoindex` and `Find`; (2) general interpretation and classification and the complete Context Builder; (3) memory
and the web in the packet; (4) external connectors through MCP, one at a time.

### 6.1 Stage 1: the core, code and files, pre-LLM

| Phase | Content | New files | Files touched | Done when | Days | Depends on |
|---|---|---|---|---|---|---|
| **U0. Skeleton, trace, request state** | `UioeCtx`, `UioeSession`, `UioeRequest`; the trace writer and rotation; the `katana uioe` dispatcher with `connectors` and `state`; the request-state globals (`g_rs`, `g_rc`, `g_intent_*`, `rr_*`, `rp_*`, `g_place_dirs`) behind `uioe_state_*` functions with **no behaviour change** | `types.c`, `trace.c`, `state.c`, `cli.c` | `src/katana.c`, `src/tools/common.c`, `src/agent/loop.c`, `src/agent/locate.c`, `src/app/main.c` | the build passes; `katana uioe state` in a session lists the ranges a `Read` just delivered; the session file of a scripted run is byte-identical to before | 2 | nothing. **Can start now** |
| **U1. Interpret, Classify, Route for code and files** | the registry; `UioeAsk` with the code and file entities, places, relation bits; domains `code`, `files`, `chat`, `system` and the task kinds; the route plan, route confidence and gate rules; `katana uioe plan` | `registry.c`, `interpret.c`, `classify.c`, `route.c` | `cli.c` | `katana uioe plan` prints the forms of 4.3 and 4.4 for the owner's real requests (those in `~/.katana/desktop/sessoes/*.json`); the owner reads the table and corrects cues | 3 to 4 | U0. **Can start now** (new files only) |
| **U2. Structured API of the coder** | 3.8.4: `sy_q_*`, `fl_q_*`; the `coder_*_main` functions become wrappers; `sym_tool_run` stops capturing stdout; `SyView` reusable inside one `UioeCtx` | - | `src/coder/symquery.c`, `flow.c`, `symtool.c` | `katana coder def|refs|callers|callees|impact|flow` and `katana tool Symbols` print the same bytes as before on the katana tree (diff of saved outputs) | 3 to 4 | must run **between lifecycle waves** (waves 1 and 4b edit `symquery.c` and `symtool.c`): after wave 1 is merged |
| **U3. Retrieve and Orchestrate; code and file connectors** | the state machine; pipelines locate, usage, impact, flow, config, error, how-to (documents), overview; the evidence graph; tiers and fusion; verification; deadlines, breaker, degradation ladder; the cache; connectors `code.*`, `file.*`, `place.resolve`, `docs.*`; `katana uioe explain` | `orch.c`, `pipes.c`, `graph.c`, `fuse.c`, `verify.c`, `budget.c`, `cache.c`, `conn_code.c`, `conn_file.c` | `cli.c`, `registry.c`; new `src/agent/uioe_agent.c` (registers the agent-side connectors) | `katana uioe ask --raw` on the greymod question lists a chain containing `grey-channel.mjs:61 -> channels.mjs:33`; with the store folder renamed the same command falls back to scanning files and says so; `explain last` replays it | 6 to 7 | U1, U2; works with today's `sym_ensure_index`, better with `index_ensure` (lifecycle phase 1) |
| **U4. Context Builder v1 and the pre-LLM hook (`pack`)** | `ctxb_build` with the format of 4.6 for the Qwen target: select, order, compress by templates, trim, render, record; the limits form; `uioe_prepare`; the hook in `loop.c` replacing `ds_select` and `place_note` when UIOE is on; the tool-guide sentence; compaction recognises old blocks; buffer review | `ctxb.c`, `api.c` | `src/agent/loop.c`, `src/agent/context.c`, `src/agent/prompt.c`, `uioe_agent.c` | `katana uioe ask` prints the block of 4.6.2 for the greymod question; a `-p` run with `KATANA_UIOE=pack` shows the block in the session file for a request that names a symbol and none for "ok, continue"; with `off` the session is byte-identical to before | 3 to 4 | U3 |
| **U5. Facades (`full`)** | `Find` default mode through the same engine with shared state; `mode=refs` with a sentence interpreted; the `Grep` phrase note; `Read` terms from the request record; the neighbour note from the evidence graph; the plan nudge by task kind; the repeat-table fixes if accepted | - | `src/tools/find.c` (`ct_findtool` only), `findgraph.c` (`fg_neighbor_note` only), `read.c`, `grep.c`, `src/agent/loop.c` | with `KATANA_UIOE=off` or `pack` the tool tests print what they print today; with `full`, a `Find` after a block returns only what the block did not show | 3 | U4. **After lifecycle wave 3** (phase 7 rewrites `find.c`): the facade is written once, on the unified `Find` |

Stage 1: about 20 to 24 agent-days (estimate). The first usable slice is U0 to U3 plus `ctxb.c`: `katana uioe ask`
from the command line, no model, no agent change. The owner can read blocks for his own questions and correct the
routing before anything reaches the model. That is the review point this design asks for.

**Status of U0 and U1 (built 2026-11-23, compiled, nothing measured, not run in a live agent session).** Files: `src/uioe/types.c`,
`trace.c`, `state.c`, `registry.c`, `interpret.c`, `classify.c`, `route.c`, `cli.c`; one `uioe_loop_state` function in `loop.c`, the `/uioe`
slash command and the `uioe` entry in `main.c`. `katana uioe ask|plan|connectors|state` work with no model; the loop does not call UIOE
(`KATANA_UIOE` defaults to `off`). Differences from the text above, decided by the implementer and recorded in each file header: the request
state is read through one snapshot (`uioe_req_sync`) instead of being moved; entities are recognized by shape or on disk, not resolved
through the index (so the "entity resolved" signal is weaker than 3.5.3 says); `gap` is not computed (it needs the index); the trace is
not yet masked for secret shapes; only G1 to G3 are decided before retrieval.

**Status of U3 (built 2026-11-23, compiled, nothing measured beyond single manual runs, not run in a live agent session).** Files: `src/uioe/evgraph.c`
(graph, fusion, order), `verify.c`, `conn_file.c` (`file.grep`, `file.glob`, `place.resolve`, `docs.search`, `docs.select`), `conn_code.c` (`code.search`, `code.def`,
`code.callers`, `code.refs`, `code.callees`, `code.outline`, `code.deps`, `code.impact`, `code.flow`, all over one `SyView` per request), `budget.c` (deadlines,
breaker, the one function that runs a connector), `pipes.c` (the pipelines), `orch.c` (the state machine and the gate G4 to G6), `evrender.c` (text and JSON output,
the trace of the retrieval); `cli.c` gains `ask` (now executes the route), `plan` (U1 only), `explain`, `session`. `KATANA_UIOE=off` is untouched: nothing in
`src/agent/` or `src/tools/` changed. What exists beyond the text above, or differs from it (decided by the implementer):

- **Chain by best total.** The flow answer path is built from the best link of each of up to 6 file pairs, each followed up to 4 hops (key link, importer with a
  symbol that matches the terms, callee); the chain with the highest sum wins and the other next steps are listed as branches. Anchors are the union of two
  searches (the request's words, and their English equivalents from the knowledge base) and **all** their files are expanded (up to 16, not 5): word rank is right at
  rank 1 so rarely that a narrow anchor set loses the answer, and an expansion costs about 0.1 ms.
- **The gate is recomputed from scratch after retrieval**, and a packet never rests on word ranking alone: without a verified structural finding with confidence
  >= 75 on the path (flow) or on a named entity (other kinds) the confidence is capped at 69. How-to and overview requests are the exception: a reread document line
  that matches two or more terms is their finding. `gap` is still not computed (the coder does not expose the document frequency of a term).
- **Soft deadlines.** In-process connectors cannot be interrupted: a call over 4x its `default_ms` (index class) is `UR_TIMEOUT` with its data kept, no retry,
  and counts in the breaker; the request deadline is checked before every call. `code.impact` has `default_ms` 250 and runs last, at depth 3 only when more than
  600 ms is left, else depth 2 (depth 3 took 0.5 s on the katana tree).
- **No index = no index built.** The UIOE never builds an index on its own; with no store it scans files (degradation 5: K = 3, literal search of the 3 most specific
  terms). `--index` builds first, `--no-refresh` reads the store as it is. With a store, `index_ensure` runs first, outside the request clock.
- **Not built in U3:** the result cache (3.6.3), `session.facts`, `memory.recall`, `history.search`, `howto.recipes`, `skill.match` (U8), the web (U9), `docs.select`
  outside an agent session (it needs the model's tokenizer; the code path exists in `conn_file.c` and was never run), `code.rank`, `code.tree`, the commands/`cmd`
  step of how-to, the Context Builder and the hook in the loop (U4). Two test hooks exist: `KATANA_UIOE_FAULT="connector=timeout|error|partial,..."` and `KATANA_UIOE_SCORES=N`.

### 6.2 Stage 2: general interpretation and the complete Context Builder

| Phase | Content | Files | Done when | Days |
|---|---|---|---|---|
| **U6. General Interpret and Classify** | dates and periods, amounts, proper names, constraints, ambiguity; all domains of 3.4 with their cue tables; complexity; the retrieve rules C1 to C7; risk bits; follow-ups that inherit an open investigation | `interpret.c`, `classify.c`, new `dates.c` | `katana uioe plan` on 20 non-code requests written by the owner shows the periods normalized and the domains named; requests for uninstalled domains produce the "no connector installed" limits line | 3 |
| **U7. Context Builder, complete** | deduplication against the request text and the request state; budgets per source; the `Findings` skeleton; target profiles (Qwen now; the Gemma 4 profile as a table entry, untested by the owner's rule); the 25%-of-free-context rule; the desktop marker with the block's size | `ctxb.c`, new `target.c`, `desktop-qt/` (display only) | a request that already names a file gets a block that does not explain the file back; a second request in the same session does not quote ranges already shown | 2 to 3 |

### 6.3 Stage 3: memory and the web in the packet

| Phase | Content | Files | Done when | Days |
|---|---|---|---|---|
| **U8. Memory, earlier sessions, session facts, recipes, skills** | connectors `memory.recall`, `history.search`, `session.facts`, `howto.recipes`, `skill.match`; pipelines history and the local steps of findings; dates on every memory line; the secret mask | new `conn_local.c`; `uioe_agent.c` | `katana uioe ask "o que combinamos sobre a porta do servidor de testes"` returns the memory line with its date and no code result | 2 to 3 |
| **U9. The web connector** | `web.search` and `web.fetch` behind the policy of 3.8.6 (`KATANA_UIOE_WEB`), sending interpreted terms; pages as evidence nodes with URL and fetch time; reuse of the page memory of `webnav.c` | new `conn_web.c`; `uioe_agent.c` | with the switch off the block proposes `Search the web for "..."` in `Next`; with it on, the trace line `sent:` shows exactly the terms that left | 2 |

### 6.4 Stage 4: external connectors through MCP, one at a time

| Phase | Content | Files | Done when | Days |
|---|---|---|---|---|
| **U10. The connector interface** | connector cards (parser, `katana uioe connector check|list|show`), the `mcp:<server>` adapter over `mcp_call`, named queries with typed arguments, the result schema, policy (`auto`, `sends`), rate limits and their counters, the cache rule for sensitive sources, entity lists from a connector | new `conn_ext.c`, `card.c`; `uioe_agent.c`; `docs/mcp.md` | with a small test server (a script that answers two named queries from a text file) and its card, `katana uioe ask` produces a `Findings` block with source and date, and `katana uioe connectors` shows the rate counters | 4 to 5 |
| **U11. Databases** | the first real domain: the card conventions for a read-only database server (schema query, named queries, the "schema instead of answer" step) | a card and a document; the server itself is a third-party plugin, outside the repository and outside this estimate | the case of 5.2 runs against a real server the owner chooses | 1 to 2 for katana's side |
| **U12. Legal sources, then financial APIs** | cards and cue lists per domain; the conflict rule between sources; date handling (a quote is worthless without its time) | cards and documents | one question per domain answered with dated findings | 1 to 2 each for katana's side |

### 6.5 Across the stages

| Phase | Content | Depends on | Days |
|---|---|---|---|
| **U13. Curriculum v8 families and documents** | the families and format changes of 4.9; `docs/tools.md`, `docs/curriculum.md`, `docs/handoff.md`, `docs/plan.md`, README environment table | the format of 4.6 frozen (after U5 and U7); together with the lifecycle's v8 requirements | 3 to 4 |
| **U14 (optional, each switched off)** | (a) the local model as expander with `terms.tsv`; (b) `lsp.confirm`; (c) worker threads for the expansion steps | (a) the side-generation question of `docs/code-intelligence.md` 5.3; (c) lifecycle phase 5 and U2 | (a) 2 to 4, (b) 1, (c) 2 to 3 |

Whole design, U0 to U13: about 39 to 50 agent-days (estimate), of which stage 1 is about half. The third-party
servers of stage 4 are not in that number.

### 6.6 Dependencies on the indexing phases

From `docs/code-index-lifecycle.md` 5.1:

| Lifecycle phase | What UIOE uses from it | Without it |
|---|---|---|
| 1. Cheap queries (`index_ensure`, cached views, `index_touch`) | Route's preconditions with a budget; cache invalidation; one `SyView` per request | U3 calls `sym_ensure_index` (synchronous, as today) and stamps files itself |
| 2. Roots by init | the root of a request | the working folder or the cited folder, as today |
| 3. Crawler and levels | the index level each connector needs, partial scopes | every connector is "ready or absent" |
| 4. Freshness (`fresh_check`) | verification re-parses one changed file | "line not verified" plus the grep fallback |
| 6. Status lines, `tree`, state map | the text of the limits; `code.tree`; the overview pipeline | overview from a directory listing |
| 7. Unified `Find` | `code.rank` reads the store; the facade is written once | U5 waits |

**What can start today**: U0 and U1 (new files, plus small wrappers in `common.c`, `loop.c`, `locate.c`); the card
format of U10 as a document. U2 needs a slot between lifecycle waves. U3 and U4 are new files except the hook.

### 6.7 Manual acceptance scenarios (D9)

No measurement campaign. These are the cases of section 5, written so the owner can run each one on the desktop
with `KATANA_UIOE=pack` and look at three things: the block shown in the session, the number of tool calls until
the answer, and the request's record in `~/.katana/uioe.log` (or `katana uioe explain last`).

| # | Type this | Expect in the block | Expect in the trace | Pass when |
|---|---|---|---|---|
| S1 | the greymod question of 1.1, from the home folder | the block of 4.6.2: three steps, the `register` link, `Next: Read bridge/server.mjs ...` | `kind flow (R70)`, the folder located, decision `G4` | the model answers in at most 3 steps and cites lines 61 and 33 |
| S2 | "quem chama rr_check e o que quebra se eu mudar?" in the katana folder | the definition, the single call site, the callers by depth, `rr_after` and `rr_reset` under `Links` | `kind impact (R50)`, entity resolved at the exact step | the model answers without calling `ToolSearch` |
| S3 | "onde esta a configuracao do orcamento do docsel?" | where `KATANA_DOCSEL_TOKENS` is read and its default; "no file here sets it" | `kind config (R60)` | at most 2 steps |
| S4 | paste `error: unknown mode "refs"` and ask why | the literal's location in `ct_findtool`, the condition above it | `kind error (R40)`, literal found | the model reads the guard and answers |
| S5 | "como instalo o katana?" | README lines as `Evidence`, no code steps | `kind how-to (R90)`, `docs.select` or `docs.search` | 1 step |
| S6 | "o que combinamos sobre a porta do servidor de testes?" (after stage 3) | the memory line with its date; no code result | `domain memory`, `kind history (R30)` | 1 step |
| S7 | "oi, tudo bem?" and then "ok, continue" | **no block** | `retrieve 0 (C1/C2)`, decision `G1` | the session file has no block for these turns |
| S8 | a question with a symbol name that does not exist | the limits form: not found in the indexed set, similar names | decision `G5` | the model does not search again for the name |
| S9 | the same greymod question with `KATANA_UIOE=off` | no block; today's place note | no record | the session is what it is today |

## 7. Risks and honest uncertainties

1. **The local model may ignore the block, or over-trust it.** Nothing shows that Qwen3.6-35B-A3B, untrained on
   this format, reads a 300 to 1500-token block and acts on it; in session 3 of 1.1 it replied to a one-line note
   in words. Until v8 is trained the only instruction is the block's first line and one sentence in the tool guide.
   Pre-LLM first (D1) means this risk is met on day one, not after the facades. The protections are the default
   `off`, the gate, the review through `katana uioe ask`, and the scenarios of 6.7.
2. **A wrong block is worse than no block.** The entry of a flow question is chosen from words; the research
   measured that choice as right about 4% of the time at rank 1 and 41% within the top 5. The bet is that structure
   (a verified link between two candidates) is a far better signal than rank, and that the gate keeps the rest out.
   Both are **unmeasured**.
3. **The gate may be wrong in either direction.** Its weights are invented. Too strict and UIOE injects almost
   nothing (the cost was building it); too loose and the context is poisoned. Only the owner's use will tell, and
   the only instrument proposed is the trace.
4. **The block costs tokens every time it is injected.** 1500 tokens is about 25 s of reading (arithmetic) and
   stays in the context until compaction drops it. Most blocks should be far smaller (the example is about 330).
5. **Routing errors.** Cue words are brittle: "por que" appears in requests that are not errors, "config" in
   requests about code, a capitalized word is not always a name. A wrong kind costs budget and gives a less useful
   block, not a false one; a systematically wrong kind for a common phrasing is invisible unless someone reads
   traces.
6. **Confidence numbers are not calibrated.** The values of `sy_conf` and `fl_pair` are hand-set; the caps for
   memory, earlier sessions, the web and external sources in this document are invented. They order evidence; they
   are not probabilities, and the research found that a calibrated threshold did not separate right from wrong on
   investigation questions.
7. **The general domains are designed without a single connector to test them against.** No database, legal or
   financial connector exists. The card format, the named-query idea and the `Findings` skeleton are untested
   designs; the first real connector will change them. Stage 4 is deliberately last.
8. **Deterministic interpretation of open language has a low ceiling.** Dates, amounts and names by shape in two
   languages; no syntax, no coreference beyond "inherits the open investigation". Requests phrased unusually fall
   to "no retrieval". That is the intended failure mode, and it means UIOE helps on the requests it understands and
   is invisible on the rest.
9. **Cost of keeping it.** Twelve task kinds, nine domains, cue lists in two languages, pipelines as tables, a
   curriculum family each. A reasonable outcome is that a handful of pipelines (locate, usage, flow, how-to,
   findings) earn their keep and the rest collapse into them.
10. **Global state.** Section 3.11.2 lists it. U0 wraps the request state; the coder's statics remain. Until they
    are removed UIOE is single-threaded for in-process sources, and a request that touches two projects re-roots
    the old `Find` index on each call (until the unified `Find`).
11. **Training goes stale with every format change.** The block replaces two notes that v7 teaches (`x-place`,
    `x-docs`); with UIOE on and a model trained on v7, the model meets a format it never saw. The plan freezes the
    format before v8, but the lifecycle work also changes what `Find` prints. Order is the mitigation: finish U5,
    U7 and lifecycle phases 6 and 7, then generate.
12. **Two designs in flight on the same files.** The lifecycle waves and UIOE both edit `symquery.c`, `symtool.c`,
    `find.c` and `loop.c`. The phase tables schedule around the waves; a slip in either produces merge work.
13. **Latency before the first token.** 400 ms is a target. A cold index, a network file system, a huge folder or
    an external connector can exceed it; the deadline cuts the pipeline, but `index_ensure` waits are outside UIOE's
    own clock.
14. **Privacy.** The trace is a second copy of request text on disk, masked by shape only. A connector with
    `sends: terms` receives names and dates from the request; the trace records what was sent, but nothing can
    recall it.
15. **Not examined for this document**: `src/agent/context.c` (how a block interacts with live compaction), the
    Gemma 4 path, Windows (`find.c` is a stub there), the server path (`katana serve --agent`), and how MCP servers
    behave when called before the model's first step (`mcp_init` timing).

## 8. Decisions

### 8.1 Taken (delegated by the owner on 2026-11-23, recorded here)

| # | Decision | Where |
|---|---|---|
| D1 | Pre-LLM first, facades after; one block in the user turn; the system prompt does not change per request; no new tool | 4.2, 3.9, 3.10 |
| D2 | Conditional injection: only when retrieval is useful and the route confidence is high | 3.4, 3.5.3, 4.6.4 |
| D3 | Deterministic router; the local model only as an optional expander, off by default | 3.5.1, 4.8 |
| D4 | Filesystem, web, code, memory and documents in process; financial, legal and databases only through MCP, behind the connector interface | 3.8 |
| D5 | Context Builder with a fixed section order, most relevant first and `Next` last, deduplication, a budget per source, every statement with source and confidence | 3.9, 4.6 |
| D6 | Retrieved content is data; the block is marked as the program's; secrets never enter; external connectors never receive user text unless the route decided to send it | 3.8.5, 3.8.6, 3.11.3 |
| D7 | Build order in four stages | 6 |
| D8 | `KATANA_UIOE=off|pack|full`, default `off` | 4.8 |
| D9 | No campaigns; the trace, `katana uioe explain`, and manual acceptance scenarios | 3.11.1, 6.7 |
| D10 | Curriculum v8 teaches the block and continuing from `Next` | 4.9 |

### 8.1b Decided by the owner on 2026-11-23 (answers to the open questions of 8.2)

| # | Decision | Owner's answer | Consequence |
|---|---|---|---|
| 1 | When to turn `pack` on | **as soon as U4 is ready** (not after S1 to S9 and not after v8) | `KATANA_UIOE=pack` becomes the default for code and file requests when U4 lands; `off` stays as the escape. S1 to S9 are still run, but after the switch, to find what to fix; the risk (a model not yet trained for the block may ignore it) is accepted |
| 2 | `Symbols` in the core tool list | **yes, in v8** | v8 declares `Symbols` to the model and teaches `flow` and `tree` |
| 3 | Repeat table and plan updates | changed (done: `TodoWrite`, `TaskCreate`, `TaskUpdate`, `EnterPlanMode`, `ExitPlanMode` no longer clear it; commit `0e0b9b6`) | - |
| 4 | Follow-ups inherit the open investigation | **yes, with a trace line** | 3.6.2 on |
| 5 | Escape `[katana` in every tool result | **yes** | done for the tools that bring outside content (Bash, PowerShell, web, MCP, terminal): a line that starts with `[katana` becomes `[ katana`; `Read` and `Grep` already prefix lines with the line number |
| 6 | Project memory and earlier sessions as evidence | **yes: dated, low confidence, marked as memory** | `memory.recall` and `history.search` enter the block with their date, never as a fact checked in the code; the owner's refusal of 2026-11-21 is read as "not as the way the model knows his projects", not as "never as evidence" |
| 7 | First external connector | **none for now** | stage 4 (U10 to U12) is postponed; only the connector interface (U10) stays designed |
| 8 | Teacher mode (`src/agent/teacher.c` names a product, a model and a URL) | **move to an optional external plugin** | the binary keeps a generic "teacher model" interface with no product name; the product-specific code goes to a separate process |

Note of 2026-11-23 (owner): wherever this document says "v8", read "the curriculum updated in place (v7), regenerated at the end of the UIOE construction" (`docs/plan.md` T5). There is no separate v8.

### 8.2 Still open, for the owner

| # | Decision | Options | Recommendation |
|---|---|---|---|
| 1 | **When to turn `pack` on** | after reading `katana uioe ask` outputs; after the scenarios of 6.7 on the desktop; only after v8 is trained | after S1 to S9 on the desktop with the base model. If the untrained model ignores or misreads the block, wait for v8 |
| 2 | **`Symbols` in the core tool list** | stays "by name" (one `ToolSearch` step before first use); moves to the core list | move it in v8: the measured sessions never reached `flow`, and with `--no-tool-decl` a core tool costs no prompt tokens |
| 3 | **The repeat table and plan updates** | keep (a `TodoWrite` clears the memory of reads); change (only tools that change files or run commands clear it) | change. It is the direct cause of the rereads in 1.1 |
| 4 | **Follow-ups inherit the open investigation** (3.6.2) | off; on | on, with its trace line; it is what makes "e onde isso chega?" continue instead of restart |
| 5 | **Escaping `[katana` inside every tool result** (3.11.3), not only inside the block | only the block; every tool result | every tool result: a file that contains a `[katana: ...]` line is today delivered to a model that is told to obey such lines |
| 6 | **Project memory as evidence.** On 2026-11-21 the owner refused runtime retrieval of project facts as the way for the model to "know" his projects. UIOE's memory and earlier-session connectors retrieve at run time | exclude `history.search`; include it as dated, low-confidence evidence | include, capped and dated: it is evidence for one request, not knowledge the model is expected to hold. The owner should confirm this reading |
| 7 | **The first external connector** | databases; legal; financial | databases: the schema-and-named-query design is the most testable, and the others reuse it |

## 9. Comparison with known approaches

Short and from general knowledge of these designs; no system was run or read for this document.

| Approach | What it is | What katana takes | What katana rejects, and why |
|---|---|---|---|
| **Reason-and-act loops (ReAct) and planner-executor agents** | the model alternates thought, tool call and observation; a planner model decomposes the task | the observation format matters more than the tool list; a suggested next action after each result (`Next:`) | the model as router. With a strong hosted model one more step costs seconds; here it costs 1 to 2.5 minutes (observed), so the routing a model would do in three steps is done by rules in milliseconds. The model keeps the part rules cannot do: reading evidence and concluding |
| **The language server protocol** | a per-language server answers definition, references, call hierarchy, on an exact semantic model | the query vocabulary (definition, references, callers, callees), positions as `file:line`, and "confirm when available" (`lsp.confirm`) | a server per language as a requirement: third-party programs, slow cold start, nothing for links through strings, events, routes or environment variables, which is where the measured question lived. It stays an optional confirmation plugin |
| **Code search in editors with agents** | an agent is given grep, file read and sometimes an embedding index, and iterates | the tool trio is kept and always sees everything; ranked snippets with line ranges | iteration as the search strategy, and embeddings as the index. Iteration is the slow part here; embeddings were measured without gain on this machine (`docs/investigation-engine.md` 4) and are excluded from the core by the owner |
| **Kythe and SCIP: a code knowledge graph** | a language-neutral graph of nodes (symbols, files) and typed edges, produced by per-language indexers and queried by tools | the shape of the evidence graph: typed nodes and edges, provenance per fact, language-neutral queries | compiler-accurate indexers per language (heavy, build-system dependent, third party) and a graph limited to what compilers see. katana's parsers are lightweight and its graph adds edges those systems do not have (text keys), at the price of confidence below certainty, which is printed |
| **Retrieval-augmented generation with a query router** | a classifier (often a model) sends the query to one of several retrievers; chunks are concatenated into the prompt | routing by query kind; fusion by reciprocal rank; a token budget for retrieved context | a learned or model-based router (not explainable, and another model call), vector stores, and chunks without connection. A router that picks one retriever is also the wrong shape for a flow question, which needs several engines composed in order; UIOE's unit is a pipeline, its output is a path or dated findings, not a bag of chunks, and it may decide to retrieve nothing |
| **Repository maps ranked by a graph** | a compact map of the most-referenced symbols, sent with every request | the idea that structure can be summarized cheaply (`tree`, `outline`, `main:` symbols) | sending it unconditionally: the packet is built for the request and gated |

What remains katana's own position: no AI in the core, no embeddings, deterministic and explainable, absence
reported as a first-class result, and confidence as a number with its reason instead of a label.

## 10. Relation to `docs/code-intelligence.md`

UIOE **generalizes and replaces** the orchestrator that document designed for the coder alone. The coder module
keeps everything that is about code: store, parsers, structural queries, text keys, search, knowledge base.

| From `docs/code-intelligence.md` | In UIOE |
|---|---|
| 5.1 `CoderQuestion`, 5.2 rules | **kept and extended**: `UioeAsk` and `UioeClass` (3.3, 3.4). Same tokenization, entity lookup, dropped words, gap. New: text-key and environment-variable entities, the send/receive relation bits, more kinds (impact, config, how-to, history, web, findings), domains beyond code, constraints, ambiguity, the tool-call origin, numbered rules |
| 5.3 query expansion by the model | **kept as an optional connector** (`model.expand`, U14), unchanged in its rules: terms count only after the index confirms them |
| 6.1 state machine S0 to S8 | **replaced** by P0 to P8 (3.7.1). Same forward-only shape; S1 becomes capability availability over all connectors, inside Route; S2/S4 become ANCHOR; S5/S6 become EXPAND with per-kind pipelines; a new RECALL state reads session state; CONFIRM is verification, not only the language server |
| 6.2 plan per request type | **replaced** by the pipelines of 3.7.2. EXPLAIN becomes FLOW and now leads with text-key links (which did not exist when 6.2 was written); CHANGE becomes a modifier; DEBUG becomes ERROR and keeps guards and asymmetry |
| 6.3 budgets | **kept**, with deadlines per connector, the breaker and the degradation ladder added (3.5.2) |
| 6.4 high uncertainty, 6.5 provenance | **kept**; the ban on percentages is **dropped** (superseded by the owner's cascade decision: confidence 0 to 100 is printed) |
| 7 the evidence packet | **replaced** by the format of 4.6: one `Answer path` instead of hypothesis blocks, fixed labels, a confidence number or `(guess)` per item, non-code findings in the same skeleton, produced by a separate Context Builder (3.9). The token ceilings, the trimming idea, the stable ids, the fixed header and footer for compaction are kept |
| 8.0 `katana coder ask`, `shell` | `katana coder ask` becomes an alias of `katana uioe ask` restricted to the code connectors. The interactive shell and the model's final analysis (`investigate`, C2) stay in the coder module and consume UIOE packets |
| 8.1 the agent adapter, the `Investigate` tool | the adapter is `src/agent/uioe_agent.c`; **no `Investigate` tool** (4.2) |
| 8.3 switches `KATANA_CODER_PACKET`, `_TOKENS`, `_MS` | replaced by `KATANA_UIOE` (`off`/`pack`/`full`) and the gate of 3.5.3 (the `exact` packet mode becomes the gate: only resolved entities and verified structure pass), `KATANA_UIOE_TOKENS`, `KATANA_UIOE_MS` |
| `CoderAi` (1.3) | unchanged; it is the port `model.expand` uses |

**Phase map (replaces the reading of section 9 there; that document is not edited).** Part A's phases A0 to A3
(store, parsers, exact queries, scale) are the coder's and are largely built or covered by the lifecycle waves.
**A4 "investigation without AI" (`question.c`, `investigate.c`, `packet.c`, `diagnose.c`) is no longer built inside
`src/coder/`: it becomes UIOE phases U1 (interpret, classify, route), U3 (retrieve, orchestrate, `ask`) and U4 (the
Context Builder)**; `diagnose.c`'s guards and asymmetry remain coder queries that the error pipeline calls. A5a to
A5c (the knowledge-base search) are unchanged and feed `code.search`'s conceptual channel and Interpret's concepts.
**B2 "packet before the request" becomes U4 and moves ahead of B1**, because the pre-LLM pass is now the main case;
**B1 "tools and the `Find` facade" becomes U2 (structured API) plus U5 (facades), without the separate
`Def`/`Callers`/`Callees`/`Investigate` tools** (the single `Symbols` tool already exists). C1 (expansion) becomes
U14(a); C2 (final analysis in the coder's own interface) and C5 stay where they are; **C3 (curriculum) becomes U13
and merges with the lifecycle's v8 requirements**; C4 (embedding search) remains conditional and outside the core.
New, with no counterpart there: the general domains (U6), the complete Context Builder (U7), memory and web in the
packet (U8, U9) and the external connector interface (U10 to U12). The critical path to "the agent receives a
context block for code questions" is U0 -> U1 -> U2 -> U3 -> U4, about 17 to 21 agent-days (estimate), instead of
A4 + B1 + B2.

## 11. Layers beyond this design (inventory by the owner's diagram, to be completed)

The owner's note of 2026-11-23: UIOE as a whole has several layers that katana does not yet cover. This is a
**first inventory**, from the code read for this document, organized by the blocks of the owner's diagram: what
exists (file and function), what is missing, and the size of the gap (small: days, mostly wiring; medium: a design
and one to two weeks; large: no foundation today). It is not a design. The second pass will turn it into the
layered reference model and the gap analysis, with the external references now being researched and this diagram
as the spine. Files not opened for this document are marked "not examined".

| # | Block of the diagram | Layer | What katana has today | What is missing | Gap |
|---|---|---|---|---|---|
| L0a | UIOE (all five functions) | **The orchestration core** | per-tool fragments (3.0) | sections 3.3 to 3.7 of this document | large (designed here) |
| L0b | Context Builder | **The builder itself** | `ds_select` and `place_note` as fixed notes | section 3.9 of this document | large (designed here) |
| L0c | Sources | **External domain connectors**: financial APIs, legal sources, databases | the MCP client (`src/agent/mcp.c`: `mcp_call`, cards, trust, reconnection); no connector of these domains, no card format | the interface of 3.8.5 (designed here), and every connector (third-party plugins, outside the repository) | large |
| L1 | Interpret, Sources | **Environment and world state** (operating system, repository state, running processes, installed tools, ports, what changed outside the agent) | `agent_env` (`src/agent/prompt.c`: date, system, user, shell, folder; the rest of that function was not examined), background jobs (`src/agent/jobs.c`), the command reaper (`reaper.c`), failure hints (`cmd_hint` in `diagnose.c`), the blocked-prerequisite counter (`bk_*` in `loop.c`), `katana hw` (`src/app/catalog.c`) | a queryable model of the world: branch, dirty files and recent commits as evidence; which build, test and lint commands exist and whether their programs are installed; what is listening where; changes made by the user between requests (the lifecycle's watcher sees files only). Today each fact costs a `Bash` step | medium |
| L2 | Sources, Retrieve | **Memory, short and long term** | short: this document's request and session state (3.6.2); the facts notebook and delta reads (`src/agent/facts.c`); long: `tool_remember`/`tool_recall`/`mem_tail` over one flat file; session notes (`src/app/knowledge.c`) | structure in long-term memory (scope per project, kind, validity, contradiction and expiry); automatic capture of decisions; memory about a project as opposed to about the user; consolidation of sessions into facts. Constraint already set by the owner: project knowledge is not a training target and runtime retrieval of project facts was refused as *the* solution on 2026-11-21; whether it is acceptable as *evidence* inside UIOE needs his decision | medium to large |
| L3 | Context Builder | **Context and token management** | live compaction (`agent_compact` in `src/agent/context.c`), the persistent prefix cache (`pfx_*` in `prefix_cache.c`), result sizing by free context (`g_budget` in `loop.c`), reading notes (`tool_digest`), docsel's 600-token budget | one budget across all sources (system prompt, memory tail, docsel, packet, tool results); eviction by usefulness instead of age (drop a file the evidence graph shows was irrelevant); keeping the prefix cache stable while notes are added; a per-request token ledger. `context.c` was not examined | medium |
| L4 | LLM Execution | **Execution and verification by feedback** (compile, test, lint, run) | the syntax check on `Write`/`Edit` (`code_check` in `src/agent/devtools.c`), the brief's `test:`/`done:` end note (`brief_end_note` in `brief.c`, noted in the handoff as not yet called by the loop), failure streaks (`rp_*`), `katana codebench` as an external judge | a verify step the engine runs itself after a change (the project's build and the tests the impact pipeline names), with the failure parsed into evidence (file, line, message) and fed to the error pipeline; verifying the model's *answer* against the packet (the citation check of `docs/code-intelligence.md` 8.0) | medium |
| L5 | all blocks | **Safety and permissions** | `agent_act`/`agent_confirm` with `KATANA_CONFIRM`, read and write path checks (`agent_read_access`, `agent_write_path` in `tools.c`), brief `never:`/`ask:` (`brief_guard`), plan mode (`plan_allows`), `sudo.c`, MCP trust, `defang`, `--allowed-tools` | a single policy point that every layer consults (today several checks in several files); provenance of untrusted content carried through the context (3.11.3 covers only the block); secret detection shared by tools, index and logs; an audit view | small to medium |
| L6 | all blocks | **Learning from sessions** | `katana export`/`katana learn` (`src/app/learn.c`), `katana specialize`, `katana curriculum` (scenario engine), `katana agentlog`, `tools/investigate/` (bench and trace factory) | closing the loop from UIOE traces: which routing rules misfire, which packets were followed by fewer steps, cue lists and relevance weights tuned from the owner's own sessions; a curriculum generated from real routing mistakes. Owner's rule: no measurement campaigns in this phase, so this layer waits | medium |
| L7 | Orchestrate | **Skills and workflows** | skills in the "ability" format (`src/tools/skill.c`: `needs`, steps written as a plan), `tool_howto`, `Workflow` and scheduling tools (`src/tools/workflow.c`, `schedule.c`; not examined), plans (`TodoWrite`, `loop_plan_note`) | UIOE choosing a skill or workflow as the pipeline for an intent (today `skill.match` only suggests); reusable investigation recipes per project; a plan derived from evidence (the impact pipeline's files become the steps) instead of from call counts | medium |
| L8 | LLM Execution | **Delegation and multi-agent** | the sub-agent with a clean context (`tool_delegate` in `src/agent/delegate.c`, saving and restoring the KV cache), the `Agent` tool, session mail (`src/tools/messages.c`), worktrees (`worktree.c`) | orchestrated delegation: UIOE handing a bounded reading job plus a packet to a sub-agent and merging its findings into the evidence graph; with one model in memory, delegation is sequential and its benefit is context, not time. `tools/agent.c` and `messages.c` were not examined | medium |
| L9 | Route | **Routing between models** | one loaded model; `/model` and the catalog (`src/app/launch.c`, `catalog.c`); the teacher mode, in which a larger remote model answers in place of the local one (`src/agent/teacher.c`); reasoning on or off (`KATANA_THINK`) | choosing per step: a small fast model for routing-like generations (the expander), reasoning on only for the final analysis, escalation rules. Memory holds one large model on this machine, so this is mostly about modes of the same model | large |
| L10 | all blocks | **Evaluation and observability** | `agente.log` and `katana agentlog`, the desktop session files, `katana codebench`, the 76-question bench (`docs/investigate.md`), this document's `uioe.log` and `explain` | one view per request joining the three logs (model steps, tool calls, UIOE decisions, tokens, time); regression checks for routing (`katana uioe selfcheck` is only a shape check); the desktop showing the block and the route taken | small to medium |
| L11 | LLM Execution | **Change management** (edit, diff, undo, multi-file change) | `Edit`, `Write`, `Rename`/`Replace`/`Patch` (`src/tools/multiedit.c`), undo per request (`src/agent/undo.c`, files only), `index_touch` planned by the lifecycle | change sets as first-class objects (what this request changed, as a diff the model and the owner can read); impact evidence attached to an edit before it is made; undo that also covers what shell commands changed; scope-aware rename (today textual) | medium |
| L12 | Interpret | **User interaction and intent clarification** | `AskUserQuestion`, the prompt rule on when to ask, the desktop's plan and activity display | UIOE deciding that a request is ambiguous *because the evidence is* (two unrelated high-relevance paths) and proposing the question with the two options, instead of leaving that to the model | small |

Reading the inventory against the diagram: the blocks the owner drew (UIOE, Sources, Context Builder) are where the
large gaps are, and this document designs their first version. The layers around LLM Execution (feedback from
execution, change management, delegation) and the layers that cross every block (permissions, learning,
evaluation) have working pieces today and medium gaps. Open questions for the second pass: which of these belong
inside UIOE and which are neighbours it only talks to; the order (the observed sessions suggest the core and the
builder, then execution feedback and context management, because wrong answers and slow steps are what the owner
hits); how the external references being collected map onto each block; and whether environment state (L1) should
be a source like the others or part of Interpret.
