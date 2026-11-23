# katana

**A local LLM inference engine written from scratch in C, with an agent on top.**
One compilation unit (`src/katana.c`, which includes the parts in `src/engine/`, `src/agent/` and `src/app/`), no framework, nothing linked beyond the C runtime: it reads GGUF models, runs them on
the CPU, on the GPU (Vulkan or OpenCL, loaded from the driver at run time, no SDK needed), or on both at
once, and it ships with a tool-using agent and a native desktop app.

Everything runs on your machine. The inference engine never calls an external service. The agent can
reach outside the model (web, files, shell, MCP servers) only through tools that you can see and log (the web tools use the system `curl`).

## What problem does it solve?

Modern open models are good, but the interesting ones are **bigger than a consumer GPU**. A
35-billion-parameter model needs ~20 GB at 4 bits; a laptop GPU has 4 to 8 GB of VRAM. The usual answers are
"buy a bigger card", "offload layers and accept a crawl" or "use someone else's server".

katana targets the middle ground: **run large Mixture-of-Experts (MoE) models fast on small hardware, privately,
and put a useful agent on top.**

- **MoE models only use a few experts per token** (for example 8 of 128 per layer). katana treats VRAM as a
  cache of experts instead of a place where whole layers must fit: attention stays resident in VRAM, the hot
  experts live in VRAM slots, cold experts are computed on the CPU straight from the memory-mapped file, and a
  background loader promotes experts that become hot. A token never waits for the disk.
- **Long context on a small card.** The KV cache is sized per architecture (only the attention layers keep
  it, sliding-window layers keep a ring buffer), stored in fp16, and can be saved to disk so an agent session
  resumes without recomputing its prompt.
- **A small, readable engine.** One C file you can read, change and audit, with a `selftest` command that checks the GPU and CPU
  matrix products and the whole CPU path against exact float math.
- **An agent that is part of the product, not an afterthought.** Tool calling (files, shell, terminal, git, web
  search, web fetch, persistent memory, MCP), guaranteed-valid JSON through a grammar-constrained sampler,
  context compaction, and a persistent prefix cache so the (long) system prompt is processed once.

### Measured on a laptop (RTX 3050 4 GB, i7-11800H, 64 GB RAM, Linux, Vulkan)

| Model | Size on disk | Generation |
|---|---|---|
| Gemma 4 26B-A4B (Q4_K_M) | ~16 GB | 23.7 tok/s (42.2 ms/token) |
| Qwen3.6-35B-A3B (Q4_K_M) | ~21 GB | 24.9 tok/s |
| Qwen3-30B-A3B (Q4_K_M) | 18.6 GB | 12.5 to 14.9 tok/s (ties the best configuration of the reference engine; ahead of it on a cold prompt) |

These are the figures recorded in [`docs/comparisons.md`](docs/comparisons.md) (alternating runs on the same machine,
because the machine drifts over time; the Qwen3.6 and Gemma 4 figures are from section 17 of that file). Your numbers
will differ with your hardware. What was measured, what is an estimate and what did *not* help is in
[`docs/knowledge.md`](docs/knowledge.md).

## Quick start

Linux (gcc, AVX2). Windows builds with the Visual Studio 2022 Build Tools (`build.bat`); the Linux path is the
one used and tested day to day.

```
./build.sh                                   # produces ./katana
./katana info  <model.gguf>        # architecture, tensors, context
./katana chat  <model.gguf>        # interactive chat
./katana agent <model.gguf>        # chat with tools (files, shell, web, memory, MCP)
./katana code  <model.gguf>        # the agent in coding mode (older: short coding prompt and only the coding tools)
./katana serve <model.gguf> 8080 --agent   # web page that drives the full agent (work in progress)
```

The `katana` command (console in your project, headless mode; `docs/console.md`):

```
./katana install                   # puts `katana` on PATH (a symlink in ~/.local/bin) and finds the models from any folder
cd ~/my-project && katana          # agent console in this folder: its KATANA.md, skills and MCP servers; the model is chosen for you
katana -c                          # continue the last session of this folder (-r lists them)
katana -p "list the TODOs" --output-format json   # headless: one request in, one answer out (text, json or stream-json)
katana adopt --instructions OLD.md --mcp OLD.json --skills OLD/skills   # bring over the setup a project already has
```

Models (the minimum is 24 GB of RAM; the catalog is fixed in the engine, pinned by revision and sha256):

```
./katana hw                        # RAM, GPU, VRAM, free disk
./katana catalog                   # models the engine can download, what fits here, the recommended one
./katana pull qwen3.6-35b          # download (resumes an interrupted one) and verify
```

Desktop app (Qt 6, native window, no browser):

```
desktop-qt/build.sh && ln -sf desktop-qt/build/katana-desktop-qt katana-desktop && ./katana-desktop
```

Backend selection: `KATANA_BACKEND=vulkan|opencl` (the engine picks a sensible default and falls back to
the CPU); `KATANA_CPU=1` forces the CPU. Run `./katana selftest <model.gguf>` after changing anything: it
checks the GPU and the whole CPU path against exact float math.

## What it can run

Model architectures implemented in the engine (read from the GGUF `general.architecture` field):

- **Main targets, used every day:** Qwen3.6 MoE (`qwen35moe`, hybrid Gated DeltaNet + attention) and Gemma 4 MoE (`gemma4`).
- **Also implemented:** Qwen3 MoE and OLMoE (`qwen3moe`, `olmoe`); dense Llama, Qwen2 and Qwen3 (`llama`, `qwen2`, `qwen3`);
  DeepSeek-V2 / Moonlight with multi-head latent attention (`deepseek2`); GLM-4 and GLM-4.5-Air (`glm4`, `glm4moe`); gpt-oss (`gpt-oss`).
  These get less attention than the main targets; `docs/handoff.md` says what is validated for each and what is still open.

Quantization formats are read straight from GGUF (F32, F16, BF16, the Q*_0 / Q*_K families, IQ4_NL / IQ4_XS, MXFP4 and others; `katana qtest`
checks them). Tokenizers (BPE variants and SentencePiece) and chat templates are read from the GGUF itself.

## How it works

**Weights and kernels.** Quantized weights go to VRAM in a custom layout (rows interleaved in groups of 32,
32-bit aligned words, so a warp reads neighbouring words). Generation uses fused kernels (q/k/v in one
launch, gate/up with SwiGLU, a whole one-token attention in one launch) and a recorded Vulkan command graph
that is resubmitted per token. Prompts are processed in batches with cooperative-matrix (tensor core)
kernels. On the CPU, products are integer (16-bit activations, VNNI where available) straight on the
quantized blocks, with a thread pool of our own.

**Models that do not fit.** If the model fits in VRAM the whole token runs on the GPU. If not, the first layers
run on the GPU and the rest on the CPU (one state transfer per token), or, for MoE models, the expert-cache
scheme below.

**MoE expert cache.** Every expert has a "heat" (recent use with decay). Experts resident in VRAM slots run
together on the GPU, one launch per layer. An expert outside VRAM runs on the CPU on the spot, so the token never
waits for a load; if it is hotter than the coldest resident, a loader thread brings it in and it takes a slot on a
later token. `katana pack` writes experts in the GPU layout (one file per layer) so loading is a disk read plus a
copy. Heat is saved at exit (`<model>.gguf.ktn.heat`) and the hottest experts are preloaded next time.
`katana routing` measures how concentrated expert use is per topic.

**Speculation.** When the recent context repeats, the engine verifies four tokens in one pass; output is
identical to one-by-one generation.

**Structured output.** The sampler can be constrained by a grammar so the output is always valid JSON (or follows a
JSON Schema), with the agent's tool calls restricted to real tools and their parameter schemas.

**Prefix cache.** The agent's system prompt (1,000 to 1,500 tokens with the tool list) is processed once and the
KV state is stored in `~/.katana/cache`; the next session loads it instead of recomputing.

## The agent

`katana agent <model>` (and the desktop app, which drives it) gives the model tools. The engine stays local; only the
tools reach outside.

| Tool | What it does |
|---|---|
| `run_command` | shell command, stdin closed, stderr included, timeout, exit code, head+tail of long output, live output in the desktop app, and one next-step hint when it fails (missing program or module, wrong path, permission, port in use). Runs as your normal user; when the command contains `sudo`, the sudo password dialog opens (once; the password stays only in memory) |
| `terminal` | persistent shell sessions in a pty (keeps directory and variables, shows interactive prompts); `open`, `send`, `read`, `interrupt`, `close` |
| `find_file` | finds files and folders by part of the name anywhere on the machine (home folder and the other mounted disks), best name match and shortest path first |
| `read_file`, `list_dir`, `glob`, `search_files`, `read_document` | read and find files (PDF and HTML as text); `list_dir` shows folders with their item count and files with size and date; `read_file` returns whole lines and, for a long file, says which lines came and how to ask for the next ones (`offset`, `limit`) |
| `write_file`, `edit_file` | write files; exact-snippet edits that fail if the snippet is missing or ambiguous. After each write the engine checks the syntax of the file (Python, shell, JSON, JavaScript, PHP, Ruby, when the language tool is installed) and appends the result, so the model sees a syntax error in the same step |
| `todo` | the model's own task list for long jobs: steps, then the same list with `[x]` on the finished ones; kept across context compaction |
| `job` | reads, lists or stops a command started with `run_command` and `background=true` (servers, long builds) |
| `delegate` | hands a large search or reading job to a sub-agent with a clean context and gets back only its answer |
| `ask_user` | one question to the user with 2 to 5 options (buttons in the desktop app, numbered in the terminal) for decisions that are only the user's; in a non-interactive run the model is told to decide by itself |
| `git` | status, diff, log, commit, branch, push, clone... (refuses `push --force`, `reset --hard` unless `KATANA_GIT_UNSAFE=1`) |
| `web_search`, `web_fetch`, `download_file` | built-in meta-search (DuckDuckGo, Bing, Mojeek, Wikipedia; no API key, optional SearXNG); `web_fetch` returns the main text of a page and remembers it, so `offset` continues a long page, `find` returns only the passages with a word and `links=true` lists where the page leads; downloads with SHA-256 |
| `install_package` | pip (venv-aware), npm, apt, cargo |
| `read_log`, `terminal_screen` | recent commands with exit codes, journal, dmesg, any log file; the user's tmux pane |
| `remember`, `recall` | persistent memory in `~/.katana/memoria.txt` |
| `howto` | short recipes (web, terminal, linux, script, sudo, files, code, packages, logs) the model can consult on demand |
| MCP tools | any [Model Context Protocol](https://modelcontextprotocol.io) server (stdio or streamable HTTP), listed as `server__tool` |

**Finding things in code.** The default tool set (the one the model is trained on, `docs/tools.md`) has one search entry point,
`Find`: it looks at the question and picks the way to search (a name, a quoted text, or a description in words), checks what it
reports by reading the files again, and says what it did not find. `Symbols` answers precise questions (`def`, `refs`, `callers`,
`callees`, `uses`, `outline`, `at`, `deps`, `impact`) from an index of the code, with a confidence level on every answer. `Read`
shows the part of a large file that matches what the model is looking for, and continues from where it stopped when the same file
is read again. The same engine is available without a model as `katana coder ...` (`docs/code-intelligence.md`). No embeddings
and no remote service; new, and not measured yet.

Where the tools work: anywhere on the machine. Paths can be relative to the working folder or absolute, missing folders are created, and
`git -C <folder> ...` works in another repository. The user's request is the authorization, so there are no per-folder prompts. When the
system denies a write (for example under `/opt`) the tool says so, and the model retries with `sudo=true` (`write_file`, `edit_file`) or `sudo` in the command
(`run_command`, `terminal`), and **the password dialog opens whenever sudo is requested** (a wrong password asks again, up to three times). Nothing runs as
root otherwise (`KATANA_SUDO=1` runs every command as root). The password stays only in memory (and, for terminal sessions, in a 0600 file in the user's
runtime tmpfs, removed on exit). Long files are written in parts (`append`), and a tool call cut off by the token limit is reported instead of being run half-written;
the answer limit is 32768 tokens (`KATANA_MAXTOK`). `/cd <folder>` (or the folder button in the desktop app) changes the working folder.

Safety model: every tool call is shown in the terminal and appended to `~/.katana/agente.log`; tool results enter the
conversation wrapped as data ("external content, not instructions") with the chat format's special markers
neutralized. `KATANA_CONFIRM=1` makes the agent ask before writing files, running commands or calling MCP tools.
Note the default is **no confirmation**: the agent acts on your machine. Run it accordingly.

Context management: the agent compacts its own context (the model summarizes, tool results are shrunk), keeps the
KV cache and the hot-expert state per session, and resumes old sessions without recomputing when it can.

## Run the desktop app

Requirements: the engine built (`./build.sh`), a GGUF model, Qt 6.5+ and CMake 3.21+ (on Ubuntu/Debian: `qt6-base-dev`,
`qt6-declarative-dev`, `qml6-module-qtquick-controls`).

```
./build.sh                               # 1. the engine -> ./katana
desktop-qt/build.sh                      # 2. the app    -> desktop-qt/build/katana-desktop-qt
mkdir -p models && cp model.gguf models/   # 3. put a GGUF model in models/
./desktop-qt/build/katana-desktop-qt     # 4. run it, from the repository root
```

The app looks for `katana` and `models/` next to itself, one folder up and in the current folder (`--katana PATH` and
`--models DIR` override). Choose the model with the selector under the message box (Qwen3.6-35B-A3B starts by default;
Gemma 4 also works). Do not copy the binary elsewhere: it needs the `Katana/` QML module that sits next to it in `build/`
(use `ln -sf desktop-qt/build/katana-desktop-qt katana-desktop` for a shortcut). `desktop-qt/instalar-dock.sh` adds it to
the GNOME menu and dock. Linux only for now.

## The desktop app (`desktop-qt/`)

A native Qt 6 / QML application that starts `katana agent` as a child process (no browser, no HTTP server):
conversation with streaming answers, tool activity shown as expandable steps **with each command's output
(live while it runs)**, a message queue (type while the agent works; Esc stops it), saved sessions that resume with
their KV state, a context meter, an embedded terminal, a model picker, light/dark/IDE themes, right-click copy and paste,
and a sudo password dialog. See [`desktop-qt/README.md`](desktop-qt/README.md).

## Other commands

```
katana chat  <model> ["question"] [max_tokens] [temperature]   one question, or an interactive session
katana run   <model> "text" [max_tokens] [temperature]         plain completion
katana serve <model> [port]      web chat and an OpenAI-compatible API at http://127.0.0.1:8080 (chat only, no tools; the web page is a work in progress, and with --agent it drives the full agent)
katana train <model> <data.jsonl> [--train PATTERNS] [--rank R] ...   katana's own training: writes a .kspec (docs/specialization.md)
katana gradcheck <model> [T]     checks the training backward pass (training forward vs engine, gradients vs finite differences)
katana export <out.jsonl> <sessions> [--bench results.jsonl]   agent sessions -> training data
                                       --teacher: only the answers a teacher gave in the agent (/teacher on)
katana learn <model> [--rounds N] [--repeat K] [-- train options]   learns from its own use: solve, keep what passed, train, prove
katana agent <model>             chat with tools
katana code  <model>             coding mode: the same agent with a short programming prompt and only the coding tools
katana mcp                       start the configured MCP servers and list their tools
katana coder <sub>               code intelligence without a model: index, search, symbols, def, refs, callers, callees, uses, outline, at, deps, impact, kb, status, verify, compact (`katana coder` lists them)
katana info | bench | selftest | micro | tune | pack | verify | quantize | qtest | routing | sparsity
katana tool <name> '<json>'      run one agent tool without a model (testing)
```

Chat commands: `/new` (new conversation), `/system <text>` (system prompt), `/save <file>`, `/cd <folder>` (agent), `/plan` and `/go` (agent: read-only plan mode, then approve and execute), `/undo` (agent: puts back the files changed by the last request), `/help`, `/exit`;
Ctrl+C interrupts the current answer without leaving.

`katana serve` listens on 127.0.0.1 by default, with the page embedded in the binary. Each connection gets its own
thread; the model generates one answer at a time and the other requests wait in arrival order (the KV cache of the
previous request is reused when the new prompt starts with the same tokens). `GET /health` answers without a key.

**OpenAI-compatible API** (`/v1/models`, `/v1/chat/completions`), for editors, web interfaces and client libraries
that already speak that format:

```
curl localhost:8080/v1/chat/completions -H 'Content-Type: application/json' \
  -d '{"model":"katana","messages":[{"role":"user","content":"hi"}],"stream":true}'
```

Supported: streaming (SSE, `stream_options.include_usage`), `max_tokens` / `max_completion_tokens`, `temperature`,
`top_p`, `stop` (up to 4), `seed`, `response_format` (`json_object`, `json_schema`), content as a string or a list of
text parts, roles `system`/`developer`/`user`/`assistant`/`tool`. Models that reason (`<think>`) return the reasoning in
`reasoning_content`, apart from `content`. Not yet: `tools` (native tool calling), images, `n` > 1; those requests get
a 400 that says so. The model name in `/v1/models` is the file name without `.gguf`; the `model` field of a request is
echoed but does not select anything (one model per server).

**Other machines.** `KATANA_HOST=0.0.0.0` (or one of the machine's addresses) opens the server to the network and
requires `KATANA_API_KEY` (16 characters or more; the server refuses to start without it). Requests from other
machines need `Authorization: Bearer <key>` on every path except `/health`; with a key set, `/v1` asks for it from the
local machine too. Traffic is plain HTTP: outside a trusted network, put it behind a TLS proxy.

```
KATANA_HOST=0.0.0.0 KATANA_API_KEY=$(openssl rand -hex 24) katana serve models/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf 8080
```

Testing without a real model: `python3 tools/tinymodel/make_tiny_gguf.py /tmp/tiny.gguf` writes a 0.5 MB model with
random weights (nonsense text) that exercises the server, the APIs and the queue in a second.

The original endpoint:

```
curl -N -X POST localhost:8080/api/chat -d '{"messages":[{"role":"user","content":"hi"}],"max_tokens":200,"temperature":0.7}'
```

The reply is an SSE stream: `data: {"token":"..."}` per token and a final
`data: {"done":true,"tok_s":...,"tokens":N,"prompt_tokens":N,"cached":N,"ctx":N,"ctx_max":N}`. `response_format`
accepts `{"type":"json_object"}` or `{"type":"json_schema","schema":{...}}` for guaranteed JSON.

### MCP connectors

Configure servers in `~/.katana/mcp.json` (or the file in `KATANA_MCP_CONFIG`):

```
{"mcpServers": {
  "files":  {"command": "npx", "args": ["-y", "some-mcp-server"], "env": {"KEY": "value"}},
  "remote": {"url": "https://example.com/mcp", "headers": {"Authorization": "Bearer ..."}}
}}
```

Protocol version `2025-06-18` (compatible versions accepted); tools only (no resources, prompts, sampling or OAuth);
60 s timeout per call; calls are logged to `~/.katana/mcp.log`; results are treated as data. `tools/mcp-teste/servidor.py`
is an offline test server.

### Guaranteed JSON

`KATANA_JSON=1 katana run <model> "List 3 fruits as JSON" 80 0.8` prints a single valid JSON object;
`KATANA_JSON='{"type":"object","properties":{...}}'` constrains it to a schema (`type`, `properties`, `required`,
`items`, `enum`, `const`). In the agent the grammar switches on when the model opens a tool call and restricts the name
to the real tools and the arguments to the tool's schema. `katana jsontest` runs the grammar's unit tests.

## Environment variables (most useful)

| Variable | Effect |
|---|---|
| `KATANA_BACKEND=vulkan\|opencl` | choose the GPU backend (default: automatic, falls back to CPU) |
| `KATANA_CPU=1` | do not use the GPU |
| `KATANA_GPU_LAYERS=N` | at most N layers on the GPU |
| `KATANA_VRAM_PCT=N` | share of VRAM the engine may use (default 92) |
| `KATANA_CTX=N` | maximum context (agent default: 131072 for Qwen3.6 and Gemma 4) |
| `KATANA_KV16=0/1` | force the KV cache to float32 or float16 |
| `KATANA_THREADS=N` | CPU threads |
| `KATANA_EXPERT_MB` | MoE: cap the expert cache in VRAM |
| `KATANA_SWAP` | MoE: how much hotter than the coldest resident an expert must be to take its slot |
| `KATANA_NOHEAT=1` | MoE: do not save or read heat between sessions |
| `KATANA_SPEC=0` | disable repetition speculation |
| `KATANA_DEBUG=1` | per-kernel GPU profile and per-type CPU product profile |
| `KATANA_WEB=0` | agent: disable `web_search` and `web_fetch` |
| `KATANA_SEARX=<url>` | agent: use an existing SearXNG for `web_search` |
| `KATANA_SUDO=1` | agent: run every command as root (default: normal user, root only when sudo is requested) |
| `KATANA_CONFIRM=1` | agent: ask before writing, running or calling MCP tools (`KATANA_TRUST=0` does the same) |
| `KATANA_MAXTOK=N` | agent/chat: maximum tokens per answer (agent default 32768, chat 8192) |
| `KATANA_MEM=<file>`, `KATANA_LOG=<file>` | agent: memory file and action log paths |
| `KATANA_PFX=0`, `KATANA_CACHE_DIR=<dir>` | agent: disable or move the on-disk prefix cache |
| `KATANA_JSON=1` | `run`/`chat`: guaranteed JSON output |

The full list (kernel tuning knobs, MoE policy, debugging) is in [`docs/handoff.md`](docs/handoff.md).

## Repository map

- `src/katana.c`: the single compilation unit; it only includes the parts below.
- `src/engine/`: inference (GGUF, tokenizers, CPU kernels, Vulkan, OpenCL, MoE cache, DeltaNet, sampling).
- `src/agent/`: the layers on top of the model (tools, system prompt, terminal, MCP client, context compaction, sessions).
- `src/coder/`: code intelligence on top of the engine: segment store, search, parsers for 60+ languages and formats, symbol index and queries, knowledge base (`data/coder/kb/`).
- `src/tools/`: the agent's tools (`Find`, `Symbols`, `Read`, `Grep`, `Edit`, ...).
- `src/app/`: console, HTTP server, subcommands and `main`.
- `vk/`: single-header Vulkan compute layer (`kvk.h`) and compiled shaders; `tools/`: kernel benches
  (`vkq4k`, `vkmv`, `vkatt`, `vkdelta`...), tokenizer reference tests, tool tests (`tools/testar_ferramentas.sh`).
- `desktop-qt/`: the desktop app. `docs/`: project documentation (below). `compare.sh`: alternating A/B benchmark.
- `models/` (models) and `ref/` stay out of git.

## Documentation

| File | Read it for |
|---|---|
| [`docs/knowledge.md`](docs/knowledge.md) | goal, what the engine does, everything measured, strengths and weaknesses |
| [`docs/comparisons.md`](docs/comparisons.md) | every comparison table (against the reference engine and between katana versions) |
| [`docs/handoff.md`](docs/handoff.md) | current state, next steps, open items, how to resume work |
| [`docs/console.md`](docs/console.md) | the `katana` command: install, console in a project, model choice, sessions, slash commands, headless mode (JSON contract), `adopt` |
| [`docs/code-intelligence.md`](docs/code-intelligence.md) | the code intelligence module (`katana coder`): design, parsers, search, structural queries |
| [`docs/tools.md`](docs/tools.md) | the agent's tools and how `Find` and `Symbols` decide and answer |
| [`docs/plan.md`](docs/plan.md) | priorities and done-criteria |
| [`docs/optimization-roadmap.md`](docs/optimization-roadmap.md) | optimization techniques not yet used, expected gain and effort |
| [`docs/new-models.md`](docs/new-models.md) | DeepSeek / Kimi-family models: metadata and status |

## What katana is, and what it is not

katana is a personal, from-scratch inference engine plus a tool-using agent and a desktop front end. It exists to run large local
models on modest hardware and to be a codebase you can read end to end. It is developed on one Linux machine with a small NVIDIA
laptop GPU, and that is the only setup that is tested day to day.

It is not a drop-in replacement for a mature, widely used inference stack: it supports the architectures listed above and not
"every GGUF", the Windows paths are not compiled or tested here, and the agent runs commands on your machine without asking
unless you turn confirmation on. Performance claims live in `docs/`, always with what was measured and what is an estimate.
