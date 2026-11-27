# katana: architecture

Layers L0 to L8 with every module and every connection between them, status read from the repository on 2026-11-27. Nothing here is a new measurement: "working" means a recorded test or measurement exists; the work of the last days is "partial" (compiled, narrow synthetic checks, nothing measured on the model).

![katana architecture](img/architecture.svg)

Status is shown by color and by marker shape (filled circle working, half circle partial, dashed circle planned, crossed circle disabled) and by border (solid built, dashed planned, hatched disabled). Solid arrows are data flows that exist; dashed arrows are planned. The three flow tracks at the bottom left are the request paths (text, image, code).

## How the pieces connect

One line per arrow of the diagram.

- Apps to UIOE: the user request, an attached image and any typed URLs enter the UIOE first.
- Apps to agent loop: a message sent in the middle of a task and replies to terminal prompts (including the password dialog) skip the UIOE and go straight to the loop.
- Agent loop to apps: answer stream, tool cards, plan, context meter and terminal events.
- UIOE to agent loop: `uioe_prepare` returns the request plus one context block (at most 1500 tokens), or nothing.
- UIOE facades and the tool table: `Find` and `Symbols` are answered by the same engine the UIOE uses, in both directions.
- UIOE `memory.recall` and the agent memory: stored facts come back as dated, low-confidence evidence.
- UIOE `web.fetch` and the web tools: one source registry and router; URLs typed by the user are read before the model, search only with `KATANA_UIOE_WEB=1`.
- UIOE connectors and the code index: `code.*`, `file.*` and `docs.*` queries out, evidence with file, line and confidence back.
- Tool table and the code index: `Find`, `Symbols`, `Edit` by symbol and `LSP` calls out; answers with a confidence level and a validation report back.
- Agent loop to tokenizer: the chat-template prompt with tool declarations, history and an optional image.
- Tool declarations to the grammar mask: the call grammar built from the declared schemas constrains sampling so a call names a real tool with typed arguments.
- Sampler to agent loop: the token stream and the tool-call text.
- Agent loop and the KV/prefix cache: session KV (and DeltaNet state) saved and restored, prefix reused by hash.
- Tools and EmbeddingGemma: candidate texts out, cosine scores back; the order changes, the set does not.
- Render tool and the image engine: brief, view PNG and engine (`stable` or `flux`) out; image path and status back.
- Planned: a `CodeGen` tool would send a code spec to codegen and get code and a validation report back. Nothing in the agent calls codegen today.
- Planned: generated code would enter the file through `Edit` by symbol, validated before the write.
- Engine to hardware: quantized matvec, attention, DeltaNet and expert kernels on CPU, Vulkan or OpenCL (the language model does not run on CUDA).
- Image engine to hardware: UNet, DiT and VAE kernels on CUDA, Vulkan or CPU.
- Inside the UIOE: Interpret, Classify, Route, Retrieve, Orchestrate, Context Builder in that order; Orchestrate fans out to the connectors and their evidence returns to the Context Builder.
- Inside the engine: tokenizer ids go to the model graph, the logits pass the grammar mask and then the sampler.
- Into the model graph: the `.kspec` adapter (specialize), image embeddings (vision projector), cold-expert matvec on the CPU, expert slots in VRAM and the KV and DeltaNet state.
- Hardware: the engine and the image engine both end in the L0 backends; the loader picks one at start and the CPU is always the fallback.

## Layers

### L8. Apps

Four front ends over one agent. Console and desktop spawn the same loop; `katana serve --agent` exposes it over HTTP.

| Component | What it does and how it connects | Where | Status | Note |
|---|---|---|---|---|
| Console, `-p` | REPL in a project folder; `-p` for one request (text, json, stream-json); sessions per folder; `init`, `skill`, `mcp`, `catalog`, `pull`; `katana setup` picks an LLM and fetches extras by kind (vision, embedder, image). | `src/app/console.c`, `launch.c`, `catalog.c` | **working** | pty and headless checks with the tiny test model. `setup` and the multi-file catalog are compiled only; no download was run. |
| Desktop | Qt 6 / QML, no browser. Conversations, tool cards, plan, context meter, image mode, embedded terminal on a real pty, secret-input dialog for password prompts (the secret goes to the program, never to the model), mid-task message box. | `desktop-qt/` | **working** | Selftest with real events and a stand-in engine. The pty terminal and password dialog are new: compiled and narrowly checked. Linux only. |
| `katana web` | The desktop UI in a local browser tab with a token. | `web/`, `src/app/webui.c` | **partial** | Checked by screenshots against a stand-in engine, never with the real model. |
| `katana serve` | Chat-completions HTTP API (`/v1/chat/completions`) with client-side tools (sampler parameters accepted; `image_url` parts when a projector is loaded) and `--agent` for `/v1/agent/sessions`. | `src/app/server.c`, `openai.c`, `agentapi.c` | **partial** | Tested with tiny models and fixed output, not with Qwen3.6; one agent session at a time. |

### L7. Image engine

Reached through the `Render` tool or the desktop image mode. Cascade: view PNG, Stable preview, user approval, FLUX final conditioned on the approved preview.

| Component | What it does and how it connects | Where | Status | Note |
|---|---|---|---|---|
| Prompt engine | Router, scene compiler, engine adapters; the brief is the prompt, with a token budget per engine. `katana render spec`, `generate --nl`. | `src/render/sdspec.c`, `sdbrief.c`, `sdsel.c` | **working** | Stage tests; prompt quality is not evaluated. |
| Stable preview | SD 1.5 UNet + ControlNet canny + LCM, CLIP, VAE, Euler sampler, image-to-image from the view. CPU, CUDA, Vulkan. | `src/render/sd*.c`, `tools/cuda/`, `vk/shaders/sd_*.comp` | **working** | Checked stage by stage against float32 reference tensors; timings in `docs/render.md`. The SD 1.5 weights are not in the catalog until their license is reviewed. |
| FLUX final | FLUX.2 klein 4B: Qwen3-4B text encoder, 5 double + 20 single transformer blocks, VAE, flow-match sampler, up to two reference images. CPU, Vulkan, CUDA (exact and fast precision). | `src/render/fx*.c` | **working** | Timings in `docs/render.md`; the text encoder and the reference VAE encoder still run on the CPU. LoRA training (`flux-train`) is partial: one optimizer step measured, no real run. |
| Krea 2 Turbo, inpaint, outpaint | A second final engine; masks. Inpaint and outpaint are recognised in the request and run as image-to-image. | `src/render/sdspec.c` | **planned** | Not started. |
| GPU sharing | The agent holds most of a 4 GB card, so FLUX called from the agent falls back to the CPU; the desktop frees the card by closing the agent during FLUX. | `desktop-qt/` | **partial** | Open problem inside the agent. |

### L6. Codegen

Unified Code Generation Engine (codegen): a short code spec (`.kcode`, text or JSON) becomes code in several languages through the typed AST (UIR-H) and the CFG in SSA form (UIR-G), with no text templates; Python, JS/TS, C and Go backends are built. The model writes the spec, the engine generates the code. The sources are now `#include`d by `src/katana.c` (warnings silenced), but nothing in the agent calls them.

| Component | What it does and how it connects | Where | Status | Note |
|---|---|---|---|---|
| Spec parser, checker, types, inference | Parses the spec into UIR-H (typed AST), checks names, arity, scopes, returns, types, infers types. `raw` blocks (model-written, untrusted) pass through the structural validators and are marked in the report. | `src/coder/codegen.c`, `codegencheck.c`, `codegentype.c`, `codegeninfer.c` | **working** | `tools/codegen/test.sh` standalone checks recorded in `docs/codegen.md`; not re-run for this page. |
| CFG/SSA, flow analyses, optimizer | UIR-G (control-flow graph in SSA form), data-flow analyses, optimizer, operation registry. | `codegencfg.c`, `codegenflow.c`, `codegenopt.c`, `codegenreg.c` | **partial** | Built; checks exist under `tools/codegen/`, not re-run here. |
| Backends | Python (`codegenpy.c`) is the one with recorded task tests; JavaScript/TypeScript, C and Go are built. | `codegenpy.c`, `codegenjs.c`, `codegenc.c`, `codegengo.c` | **partial** | Python: working in the standalone tests; the others not exercised for this page. |
| Incremental AST, dependency graph, code cache | Edits go through `ct_edit_symbol` (`CGX_INC_KATANA`). | `codegeninc.c`, `codegendep.c` | **partial** | Built; no caller. |
| `CodeGen` tool, sandboxed runner, protocol text | The agent-side entry point and the validator that runs generated code. | `docs/codegen.md` phase 2 | **planned** | Not started. How often the model writes a valid spec is unknown: no model was run. |

### L5. Code intelligence

Per-folder index (`katana coder`, root by `katana init`), queried by `Find` and `Symbols`, written through `Edit` by symbol. Every answer states its confidence.

| Component | What it does and how it connects | Where | Status | Note |
|---|---|---|---|---|
| Index store, crawler, freshness | Immutable segments, manifest, single writer, checksums; on-demand crawler in levels N0 catalog, N1 text, N2 symbols and references, N3 links; inotify session watch; every cited file is re-read; ignore rules with reasons; `list`, `forget`, `gc`. | `src/coder/store.c`, `crawl.c`, `watch.c`, `fresh.c`, `ignore.c`, `maint.c` | **partial** | Built; not timed on a real tree. |
| Scanners | Tolerant structural scanners for 60+ languages and formats (brace, Python-like, shell, markup, config); not full parsers. | `src/coder/parse.c`, `parse2.c` | **working** | Working in tests; depth varies (Edit by symbol tested in depth on C and Python). |
| `Find` | Name and text channels, rank fusion, evidence, file graph with sparse activation; optional EmbeddingGemma reorder. | `src/coder/search.c`, `findstore.c`, `src/tools/find.c`, `findgraph.c` | **working** | Tool tests; not measured as a whole. |
| `Symbols` | `def`, `refs`, `callers`, `callees`, `uses`, `outline`, `at`, `deps`, `impact`, `flow` (links by equal text keys), `tree`. | `src/coder/symindex.c`, `symquery.c`, `flow.c`, `symtool.c` | **working** | Callers are by equal name, not name resolution. |
| `Edit` by symbol | Fresh parse, new text indented by the engine, bracket and scanner checks and the language syntax tool run before the write; a refused edit leaves the file untouched; findings graded `[structural]`, `[derived]`, `[lsp NAME]`. | `src/coder/symedit.c`, `src/tools/edit.c` | **working** | `tools/tooltest/symedit.sh`. |
| LSP diagnostics | Optional, before and after an edit. | `src/tools/lsp.c` | **partial** | Only against a fake server; no real language server exercised. |

### L4. UIOE

The layer before the model, in C, no model call: Interpret, Classify, Route, Retrieve, Orchestrate, then a Context Builder that appends one `[katana: context for this request ...]` block (at most 1500 tokens) to the user turn. Greetings, vague requests, exact-file requests, low-confidence retrieval and deadline overruns get no block. `KATANA_UIOE=pack` is the default; `off` disables it.

| Component | What it does and how it connects | Where | Status | Note |
|---|---|---|---|---|
| U0 to U4 | Context/session/request objects, trace, request state; interpret (entities, language, task kind), classify, route plan with confidence gates; orchestrator state machine, per-task pipelines, evidence graph, fusion, re-reading of cited lines, deadlines and circuit breaker; Context Builder and the `uioe_prepare` hook in `loop.c`. | `src/uioe/types.c`, `registry.c`, `interpret.c`, `classify.c`, `route.c`, `orch.c`, `pipes.c`, `evgraph.c`, `verify.c`, `budget.c`, `ctxb.c`, `api.c` | **partial** | Compiled and checked by hand (`katana uioe ask --pack`, the loop on a tiny random model). The real model was not trained for the block and never ran with it. |
| Connectors | `code.*`, `file.*`, `docs.*`, `place.resolve` over the code index; U5 facades answering `Find` and `Symbols` on the same engine; U8 `memory.recall` (memory as dated, low-confidence evidence); U9 `web.fetch` (a page as an evidence node with URL and time). | `src/uioe/conn_code.c`, `conn_file.c`, `conn_memory.c`, `conn_web.c`, `facade.c` | **partial** | URLs typed by the user are read before the model even with `KATANA_UIOE_WEB=0`; web search and the best result page only with `KATANA_UIOE_WEB=1`. The live web path was not run. |
| Not built | Result cache, `session.facts`, history and skill connectors, external connectors through MCP (financial, legal, databases), trust classes against prompt injection. | `docs/uioe.md` | **planned** | Designed only. |

### L3. Agent and tools

The loop: prompt, model, tool call, result as data, repeat until a text answer. 40 entries in `src/tools/table.c` (the only table the dispatcher accepts, plus connected MCP tools); the prompt declares 12 and lists the rest by name for `ToolSearch`.

| Component | What it does and how it connects | Where | Status | Note |
|---|---|---|---|---|
| Agent loop | Sessions (save, resume), plan reminders, round limits, context compaction with a work-state capsule, teacher mode through an external program, `-p` headless. | `src/agent/loop.c`, `context.c`, `teacher.c`, `prefix_cache.c` | **working** | Tool tests and `tools/traintest/run.sh` with a tiny model; not measured with the real model on long sessions. |
| Loop guards, mid-task message | Repeated identical calls and repeated failures get notes or a refusal (loop detection); a message sent while a task runs reaches the model right after the current tool call. | `src/agent/loop.c`, `knote.c` | **partial** | New; compiled and narrowly checked (`tools/tooltest/loopnotes.sh`), nothing measured. |
| Flows, durable state | Declared playbooks with a `[katana: next ...]` line and soft logit bias (no hard mask, no strict mode); one event-sourced `.kstate` per session projected back as the `resume` block. | `src/agent/flow.c`, `resume.c`, `resume_note.c` | **partial** | Compiled; nothing run with a model. |
| Tool table | `Bash`, `Read`, `Edit`, `Write`, `Grep` (own regex engine), `Find`, `Symbols`, `TodoWrite`, `AskUserQuestion`, `WebSearch`, `WebFetch`, `ToolSearch` declared; `ToolSearch` loads the other 28. | `src/tools/table.c` | **working** | Per-tool check scripts in `tools/tooltest/` (counts in `docs/tools.md`); the effect of this set on the local models was not measured. |
| Terminal | `Bash` runs in a pseudo-terminal; a program that prompts becomes a task for `SendInput`, answered by engine hints, then the model, then the user; secret prompts go to the user dialog and never through the model. | `src/agent/terminal.c`, `exec.c`, `src/tools/bash.c` | **partial** | New; narrow checks only. |
| Web tools | `WebSearch` (keyless meta-search through the system `curl`), `WebFetch`; registry of verified sources, router, json/feed/html/sitemap readers, quality verifier, state line saying whether a page came complete, partial or blocked. | `src/agent/websrc.c`, `web.c`, `webnav.c`, `src/tools/websearch.c`, `webfetch.c` | **partial** | New; narrow checks (`tools/tooltest/web.sh`). |
| MCP | Client for any MCP server as `server__tool` (stdio; HTTP transport only failure-checked); `katana mcp serve` exposes katana. | `src/agent/mcp*.c` | **working** | Against a Python test server. |
| Sub-agents, schedule, notify | `Agent`, `SendMessage`, `ListAgents`, `SubagentHandback`, `Workflow`, `Cron*`, `ScheduleWakeup`, `PushNotification`. | `src/tools/agent.c`, `messages.c`, `workflow.c`, `schedule.c`, `notify.c` | **partial** | Checks pass; delivery by the loop and hand-back inside a real sub-agent never exercised. |
| `LSP` | definition, references, diagnostics. | `src/tools/lsp.c` | **partial** | 39 checks against a fake server. |
| `Render` | Calls the image engine: `image`, `request`, `brief`, `engine` (stable or flux); loaded through `ToolSearch`; stable first, ask the user, FLUX only after approval. | `src/tools/render.c` | **working** | Check script with the image engine and with a fake one; GPU sharing open (L7). |
| Safety model | Every call is shown and logged; results are data, not instructions; no confirmation by default (`KATANA_CONFIRM=1` asks). | `src/agent/loop.c`, `brief.c` | **partial** | The untrusted-content gap is known and postponed (`docs/plan.md` T2). |

### L2. Specialization (offline)

Never in the request path.

| Component | What it does and how it connects | Where | Status | Note |
|---|---|---|---|---|
| `specialize`, `train`, `curriculum`, `.kspec` | LoRA-style adapters trained at night so the model follows katana's tool conventions and the context block; the `.kspec` loads into the model graph. | `src/app/specialize.c`, `train*.c`, `curriculum.c` | **partial** | Tried on tiny models only; no training data or real run in the repository. |

### L1. Inference engine

One compilation unit (`src/katana.c`), GGUF in, tokens out, no external service. Main target: Qwen3.6-35B-A3B (`qwen35moe`, hybrid Gated DeltaNet + attention, MoE); Gemma 4 26B-A4B second; Qwen3 MoE, OLMoE, dense Llama/Qwen, DeepSeek-V2 and GLM-4 are implemented with less attention.

| Component | What it does and how it connects | Where | Status | Note |
|---|---|---|---|---|
| GGUF, tokenizers, chat templates | Weights, metadata, BPE and SentencePiece tokenizers and the template from the file. The SentencePiece encoder is now O(n log n) (linked list + pair heap, ties to the leftmost); `katana toktest <gguf>` runs golden cases and a round trip. | `src/engine/model.c`, `tokenizer.c`, `qwen35.c`, `qwen4exp.c`, `src/app/toktest.c` | **working** | Goldens and round trip on vocabulary only; SPM equivalence with the old version checked on synthetic text; not yet confirmed on a real `llama` GGUF. |
| Model graphs | Dense, MoE, Qwen3.6 and Gemma 4 graphs on CPU, OpenCL and Vulkan; batched prompt with cooperative-matrix kernels. | `src/engine/forward.c`, `vk_*.c` | **working** | Throughput recorded in `docs/comparisons.md` (older measurements). |
| Quantization kernels | Reads F32/F16/BF16, `Q*_0`, `Q*_K`, IQ4, MXFP4 from GGUF; integer products on quantized blocks (AVX2, AVX-512 VNNI). `katana qtest` checks them. | `src/engine/cpu_kernels.c`, `cpu_lowbit.c`, `qtest.c` | **working** | `qtest`, `selftest`. |
| MoE expert cache | Attention stays in VRAM; hot experts take VRAM slots by heat, cold experts run on the CPU from the mapped file; a loader thread promotes experts that heat up; heat is saved between runs; `katana pack` writes experts in GPU layout. | `src/engine/vk_moe.c`, `expert_tier.c` | **working** | Measured earlier (`docs/comparisons.md`). |
| Cold-expert kernels, predictive prefetch | 4-row `Q4_K`/`Q6_K` VNNI kernel, vectorized SiLU, aligned slices; a helper thread prefetches cold experts into the CPU cache while the GPU works, using an advance router (`KATANA_MOE_PREDICT`, `KATANA_MOE_ROWS/PF/ACT/ALIGN=0` turn off). | `src/engine/cpu_kernels.c`, `expert_pf.c` | **partial** | New; compiled, synthetic check in `moebench`, nothing measured on the model. |
| KV and prefix cache | fp16 KV sized per architecture, saved to disk; system-prompt state reused across sessions. For Qwen3.6 the Vulkan small-context graph now keeps attention KV (attention layers only) and the DeltaNet state in VRAM (backend 4). | `src/engine/forward.c`, `src/agent/prefix_cache.c` | **partial** | CPU path checked by `tools/tooltest/pfx.sh`; the Vulkan backend is new and unmeasured. |
| Sampler | min-p, typical-p, repetition penalty with window, presence/frequency, DRY, Mirostat v2, logit bias, seed; wired into the console, agent and `/api/chat`. | `src/engine/sampler.c` | **partial** | Compiled; checked on synthetic logits only. |
| Grammar-constrained decoding | JSON / JSON-Schema grammar and a tool-call grammar from the declared schemas. | `src/agent/callgrammar.c`, `forward.c` | **partial** | Checked without a model; effect on a real model not measured. |
| Lookup decoding | Draft from the context, verified in one batched pass; output identical to plain generation. | `src/engine/lookup.c` | **disabled** | Off by default (`KATANA_LOOKUP=1`). Measured 0.7x to 0.8x of plain generation: on the CPU a verify pass costs about 3 plain tokens (`docs/optimization-roadmap.md`). |
| Vision | PNG/JPEG decode, resize, ViT encoder; the `mmproj*.gguf` projector next to the model produces image embeddings; integrated in console, agent and server (`image_url`). | `src/vision/`, `src/app/launch.c`, `openai.c` | **partial** | Compiled; `katana vision` test command; not part of this audit. |
| EmbeddingGemma | CPU embedder (`katana embed`), matches the reference implementation; used by tools only to reorder candidates, never to decide or exclude. | `src/app/embed.c`, `embsvc.c` | **partial** | Wired by hand into some tools; an embedder-loading race in the pool was fixed (`pool_for` now has one owner at a time) and is not re-measured. |
| Catalog, `setup`, convert, quantize | Catalog items have a kind (`llm`, `vision`, `embedder`, `image`) and may hold several files, each pinned by sha256; `katana setup` picks an LLM and the extras. `convert` turns a Hugging Face checkpoint into GGUF. | `src/app/catalog.c`, `convert.c` | **partial** | Compiled only; `convert` checked on tiny checkpoints; the converted files are not uploaded yet. |

### L0. Hardware and backends

`KATANA_BACKEND=vulkan|opencl|cuda|cpu`; the default falls back to the CPU. GPU libraries come from the driver at run time (`dlopen`); nothing is linked beyond the C runtime.

| Component | What it does and how it connects | Where | Status | Note |
|---|---|---|---|---|
| CPU | Own thread pool; quantized integer products (AVX2, AVX-512 VNNI where present). Reference for the GPU paths. | `src/engine/cpu_kernels.c` | **working** | `katana selftest`. |
| Vulkan | Main GPU path for the language models; SPIR-V compiled at development time. | `src/engine/vk_*.c`, `vk/` | **working** | Measured earlier on an RTX 3050 4 GB (`docs/comparisons.md`). |
| OpenCL | Older GPU path, kept. | `src/engine/cpu_kernels.c` | **working** | Not the main path. |
| CUDA | Driver API through `dlopen`, kernels as PTX text, no SDK; used by the image engines only (owner exception to "nothing proprietary"). | `src/engine/cu_*.c`, `src/render/sdcu.c`, `fxdit_cu.c`, `tools/cuda/` | **working** | Measured against the CPU in `docs/render.md`; the language model does not run on CUDA. |
| Windows | `build.bat`, Visual Studio 2022 Build Tools. | `build.bat` | **partial** | Not compiled or tested after the recent work. |

## Request paths

- **Text request.** User input (console, desktop, `-p`, server) to UIOE (interpret, classify, route, retrieve over the code index) to the context block or nothing to the model on Vulkan or CPU, which answers or emits a tool call (grammar-constrained, sampled) to the tool, whose result returns as data, repeated until a text answer; the answer streams back to the app.
- **Image request.** A view PNG attached, the agent or the desktop writes a brief, `Render` with `stable` produces the preview, the user approves, `Render` with `flux` produces the final image.
- **Code request, today.** The model reads with `Find` and `Symbols`, changes code with `Edit` by symbol, validated before the write.
- **Code request, planned.** The model writes a spec; codegen parses it to UIR, checks it, builds the AST, renders the language, validates it; the result goes in through `Edit` by symbol. The codegen half does not exist in the agent yet.

## Glossary

- **UIOE**: *Unified Intelligence Orchestration Engine*: the layer that runs before the model (`src/uioe/`, `docs/uioe.md`), in C with no model call, to understand a request and gather evidence. Modes: `pack` (default), `full`, `off`.
- **Interpret**: reads the request: entities (symbols, paths, folders, quoted text, error text, config keys, URLs, dates), what is asked, and the risk.
- **Classify**: decides the domain (code, files, data, research, memory, system, chat), the kind of question (locate, usage, impact, how-to, overview, error, flow) and how hard it is.
- **Route**: chooses which sources can answer (code index, files, memory, web registry) and which cannot, and says why one was left out.
- **Retrieve**: runs the queries through the engine behind `Find` and `Symbols`, re-reads the files to verify what it found and builds the evidence graph.
- **Orchestrate**: sequences the steps and respects the deadline (`KATANA_UIOE_MS`, `KATANA_UIOE_TOTAL_MS`).
- **evidence graph (`UG_*`)**: the graph retrieval builds: nodes are files, line ranges, symbols, keys, documentation lines and facts; edges are calls, definitions, uses, imports, sends, handles, links, contains, mentions. `UG_*` is the prefix of its limits and of the decision below.
- **decision (`UG_NONE`, `UG_LIMITS`, `UG_PACKET`)**: what the UIOE does with the result: `UG_NONE` injects nothing (greeting, vague request, low confidence, deadline overrun); `UG_LIMITS` injects only "not found / limits" plus the next step; `UG_PACKET` injects the full block.
- **context block (packet)**: the `[katana: context for this request ...]` text appended to the user message, at most 1500 tokens; the system prompt does not change, so the prefix cache stays valid.
- **facades**: `Find` and `Symbols` answered by the same UIOE state, so what the block already showed is not repeated by a tool call.
- **Unified Code Generation Engine (codegen)**: turns a short code spec (`.kcode`, text or JSON) into code in several languages through a typed AST (UIR-H) and a control-flow graph in SSA form (UIR-G), with no text templates. Python, JS/TS, C and Go backends are built; not yet connected to the agent.
- **UIR-H / UIR-G**: codegen's typed AST (H) and its CFG in SSA form (G).
- **`.kspec`**: LoRA-style specialization adapter; `.kcode` is a code spec, a different file.
- **brief**: the one-line text prompt the `Render` tool sends to the image engine.
- **expert cache**: MoE experts in VRAM slots by heat; cold experts run on the CPU from the mapped file.

## Status

- **working**: built, and a recorded test, selftest or measurement shows it doing its job; it may never have run with the real model.
- **partial**: built, but not measured, not run with the real model, or limited to part of the design. Everything added in the last days (pty terminal, web router and fetch connector, sampler, Vulkan prefix cache, cold-expert kernels, prefetch thread) is here: compiled plus narrow synthetic checks only.
- **planned**: designed in a document, not built.
- **disabled**: built, measured, and switched off because the measurement did not justify it.

## What this audit could not verify

- Nothing here was measured again. Figures cited in `docs/` were taken on one machine (RTX 3050 Laptop 4 GB, Linux) on earlier dates, and nothing about the changes of the last days was measured on the model.
- The engine was not rebuilt and no test was run for this page; status comes from the code present and the checks the documents record. Component names and file lists were checked against `src/` and `docs/` on 2026-11-27.
- Whether the UIOE, the tool set, the grammar, the sampler or the index improve the real model's answers is unknown: no Qwen3.6 measurement of them is recorded.
- Windows was not built after the recent work. Specialization and codegen were judged from code and notes; the codegen tests were not re-run.
- Some older notes still carry stale status lines (for example the code-index design note and the embedding notes).
