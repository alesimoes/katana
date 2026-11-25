# katana: architecture

Written on 2026-11-24 from the repository at the commit that adds this file, on the owner's decision: "write this architecture, document it on the
katana site, and show in a diagram how everything connects and what we have in each layer". It describes what is in the tree today, layer by layer,
from the bottom up, with a status for every component. It does not describe plans as if they were built: planned and disabled parts are shown as such.

Related documents (the details live there, this page is the map): `docs/knowledge.md` and `docs/comparisons.md` (measurements), `docs/uioe.md`,
`docs/coder.md`, `docs/code-intelligence-architecture.md`, `docs/tools.md`, `docs/ucge.md`, `docs/render.md`, `docs/api.md`, `desktop-qt/README.md`.

The diagram is `docs/img/architecture.svg` (also on the site, `site/public/architecture.html`):

![katana architecture](img/architecture.svg)

## 1. Vocabulary

| katana term | What it is here | Market term |
|---|---|---|
| **katana** | The whole product: engine, agent, tools, UIOE, code intelligence, UCGE, image engine and the apps. | local LLM runtime plus agent harness |
| **engine** | `src/engine/`: reads GGUF models and runs them on CPU or GPU. No external service. | inference engine / runtime |
| **agent** | `src/agent/` and `src/tools/`: the loop that lets the model call tools, with sessions and context compaction. | agent harness, tool-using agent |
| **UIOE** (Unified Intelligence Orchestration Engine) | The layer **before the model** (`docs/plan.md` T3, `docs/uioe.md`): Interpret, Classify, Route, Retrieve, Orchestrate, then a Context Builder that attaches one verified block to the user's message. Code in `src/uioe/`. | retrieval pipeline / query routing / context engineering (agentic RAG without embeddings as the front door) |
| **context block** | The `[katana: context for this request ...]` text the UIOE appends to the user's turn. | retrieved context, injected context packet |
| **code intelligence** (module "coder") | `src/coder/`: a per-folder index (catalog, text, symbols, references, links), scanners per language family, graphs, and the tools that query it (`Find`, `Symbols`). | code index and code search (ctags, LSIF, Sourcegraph-like), not an LSP |
| **scanner (per family)** | A tolerant structural scanner for a family of languages (brace languages, Python-like, shell, markup, config...), not a full parser. | lightweight parser (tree-sitter-like role) |
| **Edit by symbol** | `Edit` with `symbol`, `where`, `new_string`: the model names the symbol, the engine finds it with a fresh parse, checks the result and only then writes. | AST-aware edit, structured edit with pre-write validation |
| **UCGE** (Unified Code Generation Engine) | Subsystem that generates code from a spec instead of letting the model write the final code (`docs/ucge.md`). | spec-driven / model-driven code generation, a small transpiler with a typed IR |
| **spec protocol (currently "CSP", to be renamed)** | The compact text (or JSON) the model writes to describe what to build. The name collides with other acronyms and the owner agreed to rename it later; the code and `docs/ucge.md` still say CSP. | DSL, intermediate spec language |
| **UIR** | UCGE's typed intermediate representation: a typed AST (UIR-H) and a control-flow graph in SSA form (UIR-G, designed). | typed IR |
| **image engine** (`katana render`) | `src/render/`: Stable Diffusion 1.5 with ControlNet canny and LCM, and FLUX.2 klein, written in C. | local image generation pipeline (diffusion) |
| **Stable / FLUX** | The fast preview engine and the final-quality engine of the image cascade. | SD 1.5 + ControlNet + LCM; FLUX.2 klein 4B |
| **expert cache** | MoE experts live in VRAM slots by heat; cold experts run on the CPU from the memory-mapped file. | MoE expert offloading |
| **prefix cache** | The system prompt's KV state, saved to disk and reused by the next session. | prompt caching |
| **brief / skills / MCP** | `KATANA.md` instructions, named instruction sets, and Model Context Protocol servers. | project instructions, skills, MCP |

## 2. Status vocabulary

Status is read from the repository (code present, tests, measurements recorded in the docs). Two project rules apply: no tests or measurements were
required in the latest phase, and only Qwen3.6-35B-A3B counts as the working model (`CLAUDE.md`). So "working" has a narrow meaning here.

| Status | Meaning |
|---|---|
| **working** | Built, and a test, selftest or measurement recorded in the repository shows it doing its job. It may still never have been run with the real model; the notes say so. |
| **partial** | Built, but not measured, not run with the real model, or limited to part of the design. |
| **planned** | Designed in a document, not built. |
| **disabled** | Built and measured, and turned off because the measurement did not justify it. |

## 3. The layers, from the bottom up

Order: L0 hardware, L1 engine, L2 agent and tools, L3 UIOE, L4 code intelligence, L5 UCGE, L6 image engine, L7 apps. This is the order of
dependency (an upper layer uses the lower ones), not the order of one request: a request enters at the top and goes down through UIOE, the model and the tools.
The image engine and UCGE are side subsystems: the first is reached through the `Render` tool and the desktop, the second is not connected to the agent yet.

### L0. Hardware and compute backends

`KATANA_BACKEND=vulkan|opencl|cuda|cpu` chooses; the default falls back to the CPU. GPU libraries are loaded from the driver at run time; nothing is linked beyond the C runtime.

| Component | What it does | Where | Status |
|---|---|---|---|
| CPU | Thread pool of its own; integer products on quantized blocks (AVX2, AVX-512 VNNI where present). Runs everything when no GPU is usable. | `src/engine/cpu_kernels.c`, `cpu_lowbit.c` | working (`katana selftest`; it is the reference for the GPU paths) |
| Vulkan | Main GPU path for the language models: dense graph, MoE expert cache, Qwen3.6 and Gemma 4 graphs, batched prompt with cooperative-matrix kernels. SPIR-V compiled at development time. | `src/engine/vk_*.c`, `vk/` | working (measured: Qwen3.6-35B-A3B 24.9 tok/s, Gemma 4 26B-A4B 23.7 tok/s on an RTX 3050 4 GB; `docs/comparisons.md`) |
| OpenCL | The older GPU path, loaded from the driver. Kept; Vulkan is the one used day to day. | `src/engine/cpu_kernels.c` | working (compared in `docs/comparisons.md`; not the main path) |
| CUDA | Driver API through `dlopen`, kernels as PTX text, no SDK. **Used by the image engines only**; the language model does not run on CUDA. Decided by the owner on 2026-11-24 as the only exception to "nothing proprietary". | `src/engine/cu_*.c`, `src/render/sdcu.c`, `fxdit_cu.c`, `fxvae_cu.c`, `tools/cuda/` | working for images (measured and compared with the CPU in `docs/render.md`); Windows branch written and untested |
| Windows | `build.bat`, Visual Studio 2022 Build Tools. | `build.bat` | partial (the Linux path is the one used and tested; the Windows build was never compiled or tested after the recent work, `desktop-qt/README.md`, `docs/render.md`) |

### L1. Inference engine

One compilation unit, `src/katana.c`, includes the parts. Reads GGUF, never calls an external service.

| Component | What it does | Where | Status |
|---|---|---|---|
| GGUF loader, tokenizers, chat templates | Reads weights, metadata, BPE and SentencePiece tokenizers and the chat template from the file. Architectures: Qwen3.6 MoE (`qwen35moe`, hybrid Gated DeltaNet + attention) and Gemma 4 MoE as main targets; Qwen3 MoE, OLMoE, dense Llama/Qwen, DeepSeek-V2, GLM-4, gpt-oss also implemented. | `src/engine/model.c`, `tokenizer.c`, `qwen35.c`, `qwen4exp.c` | working for the two main targets; the others "get less attention" (`README.md`, `docs/handoff.md`) |
| Quantization | Reads F32/F16/BF16, the Q*_0 and Q*_K families, IQ4_NL/IQ4_XS, MXFP4 and others straight from GGUF; `katana qtest` checks them. `katana quantize` and `katana convert` (Hugging Face checkpoint to GGUF) exist. | `src/engine/qtest.c`, `src/app/convert.c` | reading: working (`qtest`). `convert`: partial (checked on tiny test checkpoints only, `docs/plan.md` T1) |
| KV cache and prefix cache | KV sized per architecture, fp16; saved to disk so a session resumes without recomputing; the system prompt's state is computed once. | `src/engine/forward.c`, `src/agent/prefix_cache.c` | working (`tools/tooltest/pfx.sh`; long-context measurements in `docs/knowledge.md`) |
| MoE expert cache | Attention stays in VRAM; hot experts take VRAM slots, cold experts run on the CPU from the mapped file; a loader thread promotes experts that heat up; heat is saved between runs. `katana pack` writes experts in GPU layout. | `src/engine/vk_moe.c`, `expert_tier.c` | working (measured, see L0) |
| Structured output | A grammar-constrained sampler for valid JSON / JSON Schema, and tool calls constrained to real tools and typed arguments. | `src/agent/callgrammar.c`, sampler in `src/engine/forward.c` | partial (the call grammar is checked without a model; its effect on a real model was not measured, `docs/plan.md` A9) |
| Speculative decoding by copy (lookup draft) | Draft taken from the context, verified in one batched pass; output identical to plain generation. | `src/engine/lookup.c` | **disabled** (off by default, `KATANA_LOOKUP=1` enables). Measured: no gain, 0.7x to 0.8x of plain generation on the copy-heavy tasks, because on the CPU a verification pass costs about 3 plain tokens (`docs/optimization-roadmap.md`) |
| Embedding model | EmbeddingGemma on the CPU (`katana embed`), matches the reference implementation. Used by the tools only to reorder candidates, never to decide or exclude. | `src/app/embed.c`, `src/app/embsvc.c` | partial (the engine and its reference check work; the wiring into tools was tested by hand for some tools only; `docs/embedding-usage.md` and the README still say "not connected" and are out of date on this point; Grep ordering is off by default) |
| Vision input | Image encoder for models with a projector (PNG, JPEG, ViT). | `src/vision/` | partial (a `katana vision` test command exists; not covered by this audit) |
| Specialization and training | `katana specialize`, `train`, `import`, `curriculum`: LoRA-style training of an adapter at night. Not part of the request path. | `src/app/train*.c`, `specialize.c`, `curriculum.c` | partial (built and tested on tiny models only; the curriculum is regenerated at the end of the UIOE work, `docs/plan.md` T5; no training data in the repository today) |

### L2. Agent and tools

| Component | What it does | Where | Status |
|---|---|---|---|
| Agent loop | Sessions (save, resume), plan reminders, round limits, context compaction, repeat and failure handling, teacher mode through an external program, `katana -p` headless mode. | `src/agent/loop.c`, `context.c`, `teacher.c`, `src/app/launch.c` | working (`tools/tooltest/`, `tools/traintest/run.sh` with a tiny model and a scripted teacher); not measured with the real model on long sessions (`docs/plan.md`, A series) |
| Tool table | 37 tools; the prompt declares 12 (`Bash`, `Read`, `Edit`, `Write`, `Find`, `Symbols`, `Grep`, `TodoWrite`, `AskUserQuestion`, `WebSearch`, `WebFetch`, `ToolSearch`) and lists the rest by name; `ToolSearch` loads them. | `src/tools/table.c` | working (1244 checks recorded in `docs/tools.md`); the effect of this tool set on the local models was not measured |
| `Read`, `Write`, `Edit`, `Grep`, `Bash` | Files and shell. Own regex engine for `Grep`; `Bash` keeps its folder, has timeouts and background jobs. | `src/tools/` | working (tool tests) |
| `Find`, `Symbols` | The model's two entries into the code index (L4). | `src/tools/find.c`, `src/coder/symtool.c` | working in tool tests; not run with the real model |
| `WebSearch`, `WebFetch` | Built-in meta-search without an API key (uses the system `curl`) and page text. | `src/tools/websearch.c`, `webfetch.c` | working (45 checks, `docs/tools.md`) |
| MCP client and server | Any MCP server as `server__tool`; `katana mcp serve` exposes katana as one. | `src/agent/mcp*.c` | stdio client: working (against a Python test server). HTTP transport: partial (failure path only checked) |
| `LSP` tool | Language-server operations (definition, references, diagnostics...). | `src/tools/lsp.c` | partial (39 checks against a fake server; no real language server exercised in the tool tests) |
| Sub-agents, schedule, notifications | `Agent`, `SendMessage`, `Workflow`, `Cron*`, `PushNotification`... | `src/tools/agent.c`, `messages.c`, `schedule.c`, `notify.c` | partial (checks pass; delivery by the loop and `SubagentHandback` in a real sub-agent were never exercised, `docs/tools.md`) |
| `Render` | Calls the image engine (L6): `image`, `request`, `brief`, `engine` (stable or flux). Loaded through `ToolSearch`; a capability note is added to the prompt only when the render models exist. Stable first, ask the user, FLUX only after approval. | `src/tools/render.c` | working (41 checks of `tools/tooltest/render.sh` with the image engine, 21 with a fake one); GPU conflict open (see L6) |
| Safety model | Every call is shown and logged; results are data, not instructions; `KATANA_CONFIRM=1` asks first. Default: **no confirmation**. | `src/agent/loop.c`, `src/agent/brief.c` | partial. The "lethal trifecta" gap (private data, untrusted content, a way out) is known and postponed on purpose (`docs/plan.md` T2): trust classes are planned, not built |

### L3. UIOE (the layer before the model)

Design in `docs/uioe.md`. Request flow: user input, Interpret, Classify, Route, Retrieve, Orchestrate, sources, Context Builder, model. In C, in milliseconds, with no model call.
Greetings, vague requests, requests that name the exact file, low-confidence retrieval and deadline overruns get **no block**: the model sees the request as typed.

| Component | What it does | Where | Status |
|---|---|---|---|
| U0 skeleton, trace, request state | Context, session and request objects, trace writer, `katana uioe state` | `src/uioe/types.c`, `trace.c`, `state.c`, `cli.c` | partial (built, compiled, not measured) |
| U1 Interpret, Classify, Route | Entities, language, task kind, route plan, confidence and gates, for code and files | `src/uioe/registry.c`, `interpret.c`, `classify.c`, `route.c` | partial |
| U3 Retrieve, Orchestrate | State machine, pipelines per task kind, evidence graph, fusion, re-reading of cited lines, deadlines and circuit breaker; connectors `code.*`, `file.*`, `docs.*`, `place.resolve` | `src/uioe/orch.c`, `pipes.c`, `evgraph.c`, `verify.c`, `budget.c`, `conn_*.c` | partial |
| U4 Context Builder and the hook | The block of at most 1500 tokens appended to the user's turn; `uioe_prepare` is the door in the loop. On by default (`KATANA_UIOE=pack`); `off` is the escape. | `src/uioe/ctxb.c`, `api.c`, hook in `src/agent/loop.c` | partial (checked by hand with `katana uioe ask --pack` and the loop on a tiny random model; the real model has not been trained for the block) |
| U5 facades | `Find` and `Symbols` answered through the same engine with shared state (`KATANA_UIOE=full`) | `src/uioe/facade.c` | partial (not the default) |
| U8 memory connector | `memory.recall`: the owner's memory as dated evidence | `src/uioe/conn_memory.c` | partial |
| Result cache, `session.facts`, history and skill connectors, web in the packet (U9), external connectors through MCP (financial, legal, databases), trust classes | | | planned |

Everything built in the UIOE was compiled and checked by hand; none of it was measured, and `docs/plan.md` T3 states that it was not run in a live agent session with the model.

### L4. Code intelligence

Detail: `docs/coder.md`, `docs/code-index-lifecycle.md`, `docs/code-intelligence.md`; the original design is `docs/code-intelligence-architecture.md` (its "design only" status line is old).

| Component | What it does | Where | Status |
|---|---|---|---|
| Index store | Immutable segments, manifest, single writer, checksums. Root by `katana init`, otherwise in the home. | `src/coder/store.c`, `storedocs.c`, `roots.c` | partial (built; nothing timed on a real tree, `docs/plan.md` T4) |
| Crawler and freshness | On-demand crawler in levels N0 catalog, N1 text, N2 symbols and references, N3 links; session file watch (inotify), re-reading of every cited file; transparent ignore rules with reasons; `list`, `forget`, `gc` | `src/coder/crawl.c`, `watch.c`, `fresh.c`, `ignore.c`, `maint.c` | partial (built; the `STUB` comments on a few include lines of `src/katana.c` are stale) |
| Scanners per family | Structural scanners for 60+ languages and file formats | `src/coder/parse.c`, `parse2.c` | working in tests; measured depth varies (Edit by symbol tested in depth on C and Python only) |
| Search | Name and text channels, rank fusion, evidence, optional embedding reorder | `src/coder/search.c`, `findstore.c`, `text.c`, `kb.c` | working in tool tests (`find.sh`, `search.sh`); not measured as a whole (`README.md`) |
| Symbols and graphs | `def`, `refs`, `callers`, `callees`, `uses`, `outline`, `at`, `deps`, `impact`, `flow` (links by equal text keys), `tree`; file graph with sparse activation; each answer carries a confidence level | `src/coder/symindex.c`, `symquery.c`, `flow.c`, `symtool.c`, `src/tools/findgraph.c` | working in tool tests; callers are by equal name, not name resolution |
| Edit by symbol (validated) | `Edit(file_path, symbol, where, new_string)`: fresh parse, new text indented by the engine, brackets and scanner checks and the language syntax tool run **before** writing, so a refused edit leaves the file untouched; findings graded `[structural]`, `[derived]`, `[lsp NAME]` | `src/coder/symedit.c`, `src/tools/edit.c` | working (`tools/tooltest/symedit.sh`; `tools/editbench/bench.py` compares it with the plain edit); LSP check only with a server installed |
| LSP | Optional diagnostics before and after an edit | `src/tools/lsp.c`, `symedit.c` | partial (see L2) |

### L5. UCGE (code generation from a spec)

Decision of the owner, 2026-11-24: the model does not write the final code; it describes what to build and the engine generates it. Detail: `docs/ucge.md`.

```
request -> spec (spec protocol, currently "CSP"; text or JSON) -> parse -> typed UIR -> checks -> AST -> doc -> language backend -> validation -> code -> Edit by symbol
```

| Component | What it does | Where | Status |
|---|---|---|---|
| Parser, validator, type table and unification, pretty-printer | Reads the spec (text and JSON), checks names, arity, scopes, returns, some types | `src/coder/ucge.c`, `ucgecheck.c`, `ucgetype.c`, `ucgedoc.c` | working (`sh tools/ucge/test.sh`: 60 checks pass; 15 tasks, 13 without `raw`, generated Python passes all 53 given tests). Not part of the katana build (`src/katana.c` does not include it); standalone `tools/ucge/ucgecli.c` |
| Python backend | Generates Python from the typed AST | `src/coder/ucgepy.c` | working (same tests) |
| `raw` fallback | What the spec cannot express is written by the model, treated as untrusted | same | working (in the tests); trusted-ness is graded in the report |
| Other backends (JS/TS, C, Go, Java, Rust), type inference, CFG/SSA and optimizer, operation registry, incremental AST, dependency graph, code cache, validator and sandboxed runner, `CodeGen` tool, protocol text for the model | | `docs/ucge.md` sections 2b, 13, 14 | planned (separate branches exist in the repository; not merged into this tree) |

What is **not** known: how often the model writes a valid spec. No model was run. Measured instead (without a model): the spec is about 1.16x the size of idiomatic Python, so UCGE is not justified by token count; its argument is determinism, one description for several languages, and validation before generation.

### L6. Image engine (`katana render`)

Detail: `docs/render.md`. All written in C, checked stage by stage against float32 reference tensors from the reference implementations. Reached through the `Render` tool and the desktop's image mode.

Flow: a view from the 3D model (for example a SketchUp PNG) -> **Stable** preview (fast) -> the user approves -> **FLUX** final, using the approved preview as the reference.

| Component | What it does | Where | Status |
|---|---|---|---|
| Prompt engine (UIIE) | Router, scene compiler, priority by embedding, configuration, engine adapters; `katana render spec`, `generate --nl`. The briefing is the prompt, with a token budget per engine. | `src/render/sdspec.c`, `sdbrief.c`, `sdsel.c` | working (`tools/tooltest/render.sh`); the quality of the prompts is not evaluated |
| Stable Diffusion 1.5 + ControlNet canny + LCM | UNet, ControlNet, CLIP, VAE, Euler sampler; image-to-image from a view. CPU, CUDA and Vulkan. | `src/render/sd*.c`, `tools/cuda/sdkern.cu`, `vk/shaders/sd_*.comp` | working. Measured, 1024x576 with ControlNet and init, RTX 3050 4 GB: 20 steps 25 s on CUDA, 118 s on Vulkan, 718 s on the CPU; LCM (4 steps) 6 s, 15 s, 64 s. PSNR against the CPU image 52 dB (CUDA) and 55 dB (Vulkan) at 20 steps |
| FLUX.2 klein 4B | Text encoder (Qwen3-4B), transformer (5 double + 20 single blocks), VAE, flow-match sampler, up to two reference images. CPU, Vulkan, CUDA (exact and fast precision). | `src/render/fx*.c` | working. Measured, 4 steps with a reference: 1024x576 in 86 s (CUDA exact) or 52 s (CUDA fast, which fails the 1e-3 tolerance of the selftests); 512x288 in 47.6 s on CUDA against 120 s on the CPU; 1920x1088 in 331 s. The text encoder and the reference VAE encoder still run on the CPU (about 25 s of the 1024x576 case) |
| GPU sharing with the agent | The agent holds about 3.5 GB of the 4 GB card, so FLUX run from the agent falls back to the CPU; the desktop frees the card by closing the agent during FLUX and reopens it after. | `desktop-qt/`, `docs/render.md` | partial (open problem; not solved inside the agent) |
| LoRA training of the FLUX transformer | `katana flux-train`, in C on the CPU | `src/render/fxtrain.c`, `fxlora.c`, `fxback.c` | partial (one optimizer step measured at 131 to 140 s and 24 GB; no real training run reported) |
| Krea 2 Turbo | A second final engine | | planned (not started) |
| Inpaint and outpaint | Recognised in the request, no mask: they run as image-to-image | `src/render/sdspec.c` | planned |

### L7. Applications

| Component | What it does | Where | Status |
|---|---|---|---|
| `katana` console and headless mode | `katana` in a project folder; `-p` for one request, text/json/stream-json; sessions per folder; `init`, `skill`, `mcp`, `adopt`, `install`; `pull` and `catalog` download pinned models | `src/app/console.c`, `launch.c`, `catalog.c`, `docs/console.md` | working (pseudo-terminal and headless tests with the tiny model); a real session with the owner's other projects is still listed as open |
| Desktop (Qt 6 / QML) | A native window that drives the agent: conversations, tool cards, plan, context meter, image mode, embedded terminal. Linux only. | `desktop-qt/` | working (selftest with real events and a stand-in engine; the Windows build was never compiled) |
| `katana web` | The desktop interface in a browser tab, local, with a token | `web/`, `src/app/webui.c`, `docs/web.md` | partial (checked by screenshots against a stand-in engine, never with the real model; marked work in progress on the site) |
| OpenAI-compatible server | `katana serve`: `/v1/chat/completions` with client-side tools, and `--agent` for `/v1/agent/sessions` (katana's agent runs on the server machine) | `src/app/server.c`, `openai.c`, `agentapi.c`, `docs/api.md` | partial (tested with tiny models and fixed output; not with Qwen3.6 or Gemma 4; one agent session at a time) |

## 4. How a request moves

**Text request.** User input (console, desktop, `-p`, server) -> UIOE interprets, classifies, routes and retrieves (L3, over the code index of L4) ->
Context Builder appends the block or nothing -> the model (L1, on Vulkan or the CPU) answers or calls a tool (L2) -> the result returns as data -> repeat until a text answer.

**Image request.** The user attaches a view -> the agent (or the desktop directly) writes a brief -> `Render` with `stable` produces a preview -> the user approves -> `Render` with `flux` produces the final image.

**Code request, today.** The model reads with `Find`/`Symbols`, changes code with `Edit` (by symbol, validated before writing).
**Code request, planned.** The model writes a spec -> UCGE parses it, builds the typed UIR and the AST, generates the language, validates it -> the result goes in through `Edit` by symbol.
The UCGE half of this flow does not exist in the agent yet.

## 5. What this audit could not verify

- Nothing here was measured again. Numbers are quoted from the documents named next to them, taken on one machine (RTX 3050 Laptop 4 GB, i7-11800H, 64 GB RAM, Linux).
- The engine was not rebuilt and the tool tests were not run for this page; only `sh tools/ucge/test.sh` was run (60 checks, all passed). Every other "working" rests on the tests and measurements the documents record.
- Whether any of the UIOE, the tool set or the index improves the real model's answers is unknown: no measurement with Qwen3.6 is recorded.
- The state of the older 3-token repetition draft of the dense Vulkan graph (`KATANA_SPEC`, mentioned in `README.md` under "Speculation") was not checked.
- Branches `feat-ucge-*` and other worktrees were not inspected; the UCGE rows mean this tree only.
- Documents with older status lines: `docs/code-intelligence-architecture.md` (says "design only"), `docs/embedding-usage.md` and `README.md` (say embeddings are not connected), `src/katana.c` (`STUB` comments).
