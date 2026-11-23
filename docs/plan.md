# katana: plan

Written on 2026-11-15. Each step has a measurable done criterion. Context is in
`knowledge.md`; techniques are in `optimization-roadmap.md`.

## Current goal (2026-11-21): an independent local agent, pleasant and stable to use, that other applications can use as their agent

Set by the owner on 2026-11-21; it replaces the order of the priorities below, which are from 2026-11-15 and mostly
about dense models. Only Qwen3.6-35B-A3B (and its AgentWorld variant) and Gemma 4 26B-A4B count. Each line is done
when its criterion is measured; "not measured" items stay open even when the code exists.

*2026-11-23: the teacher mode is vendor-free in the binary: an external program speaks `docs/teacher-protocol.md`; the Messages API / CLI code lives in `plugins/teacher-anthropic/`. Open: run the plugin once against the real service, and re-run the teacher steps of `tools/traintest/run.sh` and `tools/tooltest/launch.sh` on Qwen3.6.*

**A. The agent does not get stuck (first, because it is what the owner hits every day)**

| # | What | Done when |
|---|---|---|
| A1 | Calls with array arguments in XML (fixed 2026-11-21) | seen working in the desktop with the model |
| A2 | The model answers in words and closes each request (prompt rule + note at the round limit, 2026-11-21) | in a real session, most user questions get a text answer; counted from the session file |
| A3 | Insisting on an error (repeat and failure-streak handling, 2026-11-21) | no identical failing call three times in a row in a week of `agente.log` |
| A4 | A weekly reading of `~/.katana/agente.log`: calls, errors and repeats per tool, as a `katana` subcommand | the table exists and the worst tool of each week gets fixed. **Code done 2026-11-21**: `katana agentlog [file] [--days N]` (per tool: calls, failed, identical repeats; top errors; longest failure runs); checked on a synthetic log and run on the real one |
| A5 | Tests must not write to the owner's `agente.log` | **done 2026-11-21** for `tools/tooltest` (each script uses a temporary `KATANA_LOG`; checked: 0 new lines after two scripts). `tools/testar_ferramentas.sh` not checked |
| A6 | Tools written and never exercised: scheduled prompts and session mail delivered by the loop, `SubagentHandback` in a real sub-agent, `LSP` with a real server, `EndConversation` | each one run end to end once |
| A7 | Desktop shows the `NOTIFY`, `FILE` and `FINDING` markers | visible in the app. **Done in code (2026-11-21), not yet seen on a real screen**: notification line (plus a taskbar alert when the window is in the background; the engine already calls `notify-send`, so the app does not post a second desktop notification), file card with "Abrir" and "Abrir pasta" (default program), findings card (file:line, category, summary; each item opens the file, relative paths resolve against the working folder). Also: a "o agente parou sem responder" line with a "pedir resumo" button when a request ends and the last turn had only tool calls (not after Stop), and the active/skipped `.kspec` list in the settings popup with an "Abrir a pasta" button for `~/.katana/specs`. Covered by `katana-desktop-qt --selftest-standin` (24 checks against a stand-in engine, offscreen); never run with the real engine and model, and the screenshots are offscreen software renders |
| A8 | Round limit of 40 per request: a long job should continue by itself after reporting, or the limit should follow the task list | a 100-call job finishes without the user typing "continue". **Code done 2026-11-21**: blocks of 40 rounds (`KATANA_ROUNDS`), one more block while the last one wrote a file or completed a task step, hard cap 200; checked with the scripted stand-in teacher in `tools/traintest/run.sh`, not measured with a model |
| A9 | Plan first: the engine asks for a TodoWrite plan at the 3rd and 6th call of a request without one; every 5th result names the step in progress; TodoWrite's result names the next step; Bash declares `description` and the `TOOL` marker carries it | **code done 2026-11-21**, checked with the scripted stand-in teacher in `tools/traintest/run.sh`, not measured with a model; done when real sessions show plans |
| A10 | Blocked prerequisite: 3 (and 6) failures of one request around the same program or folder, with text about something missing, get a note naming it and telling the model to say what the user must provide and write the code anyway; prompt rule for the same, and a `katana code` rule to write files | **code done 2026-11-21**, checked with the scripted stand-in teacher in `tools/traintest/run.sh`, not measured with a model |
| A11 | A new user message wins over the old plan: the message is tagged when an older list has unfinished steps; the end-of-request reminder says the newest message comes first | **code done 2026-11-21**, checked with the scripted stand-in teacher in `tools/traintest/run.sh`, not measured with a model |
| A12 | Desktop shows the plan and the activities (owner: "I cannot see what it is doing or planning") | **Done in code 2026-11-21 (`desktop-qt/`), not seen on a real screen.** The activity line shows the description the model gave the call (3rd field of `TOOL`) with the command as an expandable second line, a spinner and elapsed time while running, duration and an ok / failed mark after (failed = result starting with `error:` or an exit code other than 0, same rule as the engine). The task list (`TODO`, `[x]` / `[ ]` per line: the first undone step counts as in progress, the marker has no explicit status) becomes a plan card in the conversation: actions grouped under the step that was in progress when they ran, done steps collapsed to "N ações · time", the step in progress open, pending ones dimmed, steps removed from the list kept as "descartado" with their actions, reworded steps keep theirs. The calls that only manage the plan (`TodoWrite`, `Task*`) are hidden and not counted. Engine notes (`[katana: ...]`, `[plan: ...]` at the end of a result) are cut out of the output and shown dimmed only in the expanded action. Status line and the task card: "passo 2 de 6: text · 1 min · 7 ações" while running, "6 de 6 passos, 23 ações, 4 min" at the end. Sessions store the description, mark, duration, step of each call and the plan (as an extra message), so a restored session regroups the same way; **not stored** (so not restored): the tool output, and a plan carried over from an earlier request is only grouped once the model rewrites it in the request. Covered by `--selftest-standin` (59 checks; the new request sends 3 steps, calls with and without description, a failing call with engine notes, a reworded step, a new step, a dropped step; asserts on grouping, marks, hidden notes, summary line, session file and restored session; screenshots `desktop-qt/capturas/30` to `35`, dark and light). Never run with the real engine and model; offscreen software renders only |

| A12 | Instruction files in the "brief" format, layered (`~/.katana/KATANA.md`, project, `KATANA.local.md`), `katana init`, `katana brief check/show`, `never:`/`ask:` patterns enforced on `Bash`/`Write`/`Edit`, subfolder `KATANA.md` delivered once (2026-11-21) | **code done, checked without a model** (`tools/tooltest/extend.sh`); the end-of-request note about `test:`/`done:` is written but the loop does not call it yet; not seen with a real model |
| A13 | Skills in the "ability" format: `needs` checked, plan written from `steps`, `$arg`, extra files listed, one-line skills list in the prompt, `katana skill new/list/show/remove/path/check`, `/skill`, `/<skill> args` (2026-11-21) | **code done, checked without a model**; does the model use the skills line? not measured |
| A14 | MCP made easy: `katana mcp add/remove/list/test/card`, project `.katana/mcp.json` with a trust question, cards instead of JSON declarations in the prompt, `structuredContent`, `isError`, `list_changed`, reconnection, timeouts, prompts as `/server:prompt`, `mcp__` naming and prefix (2026-11-21) | **code done, checked without a model** against a Python stdio test server; HTTP transport only checked for the failure path; the call-format constraint with cards (no JSON in `<tools>`) not checked |
| A15 | Bug found: the confirmation answer `ANS 0` counted as yes (compared 5 characters of `\x01ANS 1`) | **fixed 2026-11-21** in `agent_ask_kind` |
| A8 | Round limit of 40 per request: a long job should continue by itself after reporting, or the limit should follow the task list | a 100-call job finishes without the user typing "continue" |
| A9 | Calls valid by construction (`src/agent/callgrammar.c`, 2026-11-21): once the model opens a call, tool name, parameter names, required parameters and typed values (JSON) are constrained; string values stay raw text. XML and JSON (ChatML) and Gemma 4; GLM unconstrained. `KATANA_CALL_GRAMMAR=0` turns it off. See `docs/call-formats.md` | rejected or malformed calls per 100 in `agente.log` drop on Qwen3.6 and Gemma 4, measured before and after with the same tasks; no change in codebench pass rate. **Not measured with a real model yet** |
| A10 | Plan as plain lines (2026-11-21): `TodoWrite.todos` also takes one step per line (`[x]` done, `[>]`/`[~]` in progress, `[ ]` pending); the short declaration in the prompt now describes only that form. 6-step plan: 120 -> 68 tokens with the Qwen3.6 tokenizer (XML call), 106 -> 59 with Gemma 4 (native call) | the models use the line form in real sessions and TodoWrite errors stay at zero |
| A16 | Gemma 4 parity in the agent (2026-11-21): live compaction and `/compact` from the cache, reading notes, sub-agent, native results, notes as results or user turns, declarations equal to the template, constraint with both tool sets and off inside thought (`KATANA_COMPACT_LIVE`, `KATANA_G4_SIDE`, `KATANA_G4_PLAIN_RESULT`) | **code done, checked without the model** (`tools/traintest/gemma4_part.sh`, golden checks against the template from the GGUF); done when the real-model checks in `docs/new-models.md` (Gemma 4) pass |
| A17 | Gemma 4 prompt order: declarations must end the system turn (template), so a new folder recomputes ~1,400 of ~2,230 tokens; `KATANA_G4_ENVLAST=1` would recompute ~430 | first-start time measured both ways and a codebench pass with `KATANA_G4_ENVLAST=1`; flip the default only if it does not hurt |

**B. The model understands the request better**

The instrument for every row below is `katana codebench` (`run`, `verify`, `compare`, `list`; written in C since 2026-11-21, the Python runner is gone). Besides solved counts it records, per run and from the agent's own log, calls, failed calls, calls the engine refused, identical repeated calls, rounds, whether the last turn was text, and tokens read and written; `compare` prints them next to the solved counts.

| # | What | Done when |
|---|---|---|
| B1 | Reasoning on or off in the desktop (it starts with `KATANA_THINK=0`) | codebench and a set of ambiguous requests in Portuguese, alternating, both models |
| B2 | Reading notes (`KATANA_DIGEST`, on by default and never measured): a file read becomes a 250-word note | codebench with and without, alternating; default follows the result |
| B3 | Qwen3.6 against AgentWorld against Gemma 4 as the desktop default | same tasks, same hour |
| B4 | First set against second set of tools | handoff 0t |
| B5 | Harder tasks (the terminal and web ones are solved by every model) and requests written the way the owner writes | tasks that separate the models. **Tasks written (2026-11-21), not yet run with any model:** 12 tasks that need new code: `write-*` (6, from an empty folder), `prereq-*` (3, the code needs a command, a credentials file or a service that is not on the machine), `extend-*` (3, a new module plus changes in existing files of a 300 to 400 line project); 8 in informal Portuguese, 4 in English. They pass `katana codebench verify` (fail untouched, pass with the reference solution, no model). Open: run them on Qwen3.6 and Gemma 4, see which separate the models, and read the first runs for ambiguous hidden tests |

**C. Speed** (after measuring where the time of one token goes: GPU, CPU, waiting)

| # | What | Done when |
|---|---|---|
| C1 | CPU experts and GPU work at the same time inside a layer | tok/s against today, alternating |
| C2 | One more expert slot in VRAM (compact Q8_0) | hit rate and tok/s |
| C3 | CPU experts: several rows per call, VNNI for Q5_1/Q8_0 | ms per token against the 17 ms (Qwen3.6) and 25 ms (Gemma 4) ceilings |
| C4 | First start of Gemma 4 (43 to 72 s) | under 15 s |
| C5 | Long context on the GPU for Qwen3.6 | tok/s at 30k tokens |

**D. Specialization: one specialized model** (`docs/specialization.md`, "One specialized model")

| # | What | Done when |
|---|---|---|
| D1 | `katana specialize` in C (kinds all/tools/code/teacher/knowledge, mix, resume across nights, history), selection options in `katana import`, the no-tools-section flag in `.kspec` and merged GGUF (**built 2026-11-21**, tested on tiny models only) | first real night on Qwen3.6 finishes and `history.jsonl` has its line |
| D2 | Import of the owner's 841 MB of transcripts through `--from <root>` | time and peak memory recorded |
| D3 | Planned work (`--plan-first`, experimental) | in real sessions with the specialization, most multi-step requests start with a task list |
| D4 | The merged GGUF without the tools section against the base with it | codebench alternating, both models, no task lost |
| D5 | Training on the GPU | seconds per window on the owner's card |
| D6 | One command per night (`katana specialize <model>`, defaults and `katana.cfg` keys `transcripts`, `training-dir`, `replace`, `cut-matching`), `--wait-idle`, `--status` report, writes that fail loudly with a checked data file, fifth source `conventions` (**built 2026-11-21**, tiny models only; `katana curriculum` is another agent's work) | first real night run with only `katana specialize <model>`; the report shows the windows seen |

**E. Install and run anywhere**: AppImage and the packaging workflow executed once; Windows build compiled again;
`test_server.py` and a real client against the two models; native `tools` in the OpenAI-compatible API (**code done
2026-11-21**, `docs/api.md`: native formats, typed arguments, streaming deltas, Gemma 4 reasoning split; tested on
tiny models with a fixed raw output, not with Qwen3.6 or Gemma 4).

| # | What | Done when |
|---|---|---|
| E1 | `katana install` / `uninstall` (symlink on PATH), models folder found beside the real binary or from `models=` in `~/.katana/katana.cfg`, `katana.cfg` of the GPU found beside the binary | **done 2026-11-21**, tested from another folder through the link (`tools/tooltest/launch.sh`); open: the owner runs it for real |
| E2 | `katana` with no arguments in a project folder: model chosen by `--model`, `KATANA_MODEL`, `model=`, the desktop's last model, the catalog, the largest | **done 2026-11-21** (every branch tested with empty `.gguf` files); open: used in the owner's other project |
| E3 | Sessions per folder (`-c`, `-r`, `~/.katana/sessions/<hash>/`), history per folder, line editor with paste detection, rich console (header, tool lines, spinner, task block, Markdown, status line), `/model`, `/status`, `/clear` | **code done 2026-11-21, tested in a pseudo-terminal with the tiny model**; open: a real session with Qwen3.6 / Gemma 4 and the owner's terminal; Windows not compiled |
| E4 | `katana adopt` (instructions, MCP servers, skills of a project that has another agent's setup) | **done 2026-11-21**, tested with sample files; open: the owner's real project |

**F. Usable as the agent of other applications** (after A and B hold for a week of real use). katana is an
independent project and is tied to no application; an application that today drives a hosted agent should be able to
drive katana instead, the same way. What such a client needs is not written down yet. To decide with the owner:
whether a client talks to `katana agent` through the line protocol the desktop uses, or to `katana serve` through the
OpenAI-compatible API (which then needs native `tools`), which machine runs the model, and how a session started on
another device is kept. E's server items are prerequisites of the second path. **First version built 2026-11-21**: both paths exist over
HTTP. Client-side tools through `/v1/chat/completions`; katana's own agent through `katana serve --agent` and
`/v1/agent/sessions` (the agent runs on the server machine as a child process; one session at a time; see
`docs/api.md` and its security warning). Done when a client application drives each model through both paths in
real use; still open: which machine runs the model for a remote device, sessions kept across devices, several
sessions at once.

| # | What | Done when |
|---|---|---|
| F1 | Headless mode `katana -p` (text, json, stream-json; `--mcp-config`, `--strict-mcp-config`, `--allowed-tools`, `--max-turns`, `--session-id`...), contract in `docs/console.md` | **code done and tested 2026-11-21 with the tiny model and the scripted teacher** (`tools/tooltest/launch.sh`); open: an application drives it with a real model, a real request, and the exit codes and JSON fields are what it needs |
| F2 | A tool outside the allow-list is neither declared nor run | **done 2026-11-21**, checked with the dispatcher (`katana tool`) and end to end; open: how a real model behaves when a tool it expects is missing |
| F3 | `--strict-mcp-config` against a user file, a project file and no config | **done 2026-11-21** for the user's and the project's files as they load today; when project-level `.katana/mcp.json` merging lands, its loader must honour `KATANA_MCP_STRICT` |
| F4 | The application's real run, repeated | one week of runs counted from `~/.katana/sessions` and its own logs |

**G. Knowledge from earlier sessions** (the owner wants the model, not a lookup, to know what was done in his projects)

| # | What | Done when |
|---|---|---|
| G1 | `katana knowledge build/search/stats/corpus` and `docs/knowledge-base.md` (2026-11-21) | **done**: notes and corpus built from the real transcripts, tested without a model |
| G2 | Train a model on the corpus (`katana specialize <model> knowledge`) | the adapter trained, nights counted |
| G3 | Held-out measurement: `katana knowledge quiz` before and after training (**tool built 2026-11-21**, run on tiny models only) | mean overlap score and the count above 0.5, before and after; the expectation (names, paths, commands, not volume) confirmed or refuted |
| G4 | Keep the base current: rebuild incrementally, regenerate the corpus, retrain on the new part (**built**: it is the first half of the nightly `katana specialize <model>`) | the owner ran it once on the real acervo |


## Backlog (2026-11-23)

| # | What | Done when |
|---|---|---|
| T1 | `katana convert`: read a Hugging Face checkpoint (`.safetensors` plus `config.json`, for example `Qwen/Qwen3.6-35B-A3B`, over 60 GB in 16 bits) and write a GGUF, so the engine no longer depends on third-party GGUF files. Today katana only loads GGUF (`katana quantize` takes a GGUF as input; the model catalog downloads Unsloth's `UD-Q4_K_M`). The hard part is the tensor mapping of the `qwen35moe` architecture (experts, Gated DeltaNet, attention, norms) to the GGUF names the engine reads, and the tokenizer and metadata. Then `katana quantize` (or a dynamic mix that keeps sensitive layers in more bits) takes it to 4 bits | converting `Qwen/Qwen3.6-35B-A3B` gives a GGUF that loads and whose first tokens match the downloaded `UD-Q4_K_M` file within quantization noise. **Written 2026-11-23, checked on tiny checkpoints only** (`src/app/convert.c`, `docs/convert.md`, `tools/convert/`): streaming, f32/f16/bf16/q8_0/q6_k/q5_k_m/q4_k/q4_k_m (dynamic default), shards, fused and per-expert experts, tokenizer and chat template; a tiny checkpoint converts to tensors byte-identical to the tiny GGUF generator's. **Still open: converting the real 35B (no checkpoint on the machine), logits against the Unsloth file, memory and time on the real model** |
| T2 | **Security gap, deliberately postponed (owner, 2026-11-23): the "lethal trifecta".** The agent runs without confirmation (owner's decision) and has all three: private data (files, memory), untrusted content (web pages, files from other people, tool output) and a way out to the network (`Bash` with curl, `WebFetch`, MCP). Text in a page or file can steer it into leaking a file. The UIOE research (`docs/research/uioe-context-memory-routing.md`, sections on security) proposes three protections: (1) **trust classes** for everything the UIOE retrieves (user, local project, third-party file, web, external connector); (2) **route pinned before reading**: which sources to consult is decided before any retrieved content is read, so content cannot change the route; (3) **lower the outgoing privileges when the session is contaminated by web content** (no outgoing `curl`/POST of local files, no `WebFetch` with local data in the URL, MCP write tools off), without asking the user anything. None of this is implemented and none is planned for now; it must not be forgotten | a session that has read a web page cannot send a local file out, and the trust class is visible in the log. **Not started, postponed on purpose** |
| T3 | **UIOE** (Unified Intelligence Orchestration Engine): the pre-LLM layer in the owner's diagram (user input, then Interpret, Classify, Route, Retrieve, Orchestrate, then sources, then Context Builder, then LLM execution). Design: `docs/uioe.md`; references: `docs/research/uioe-*.md`; decisions of 2026-11-23 in the owner's memory (pre-LLM injection of one `[katana: context ...]` block, `Find`/`Symbols`/`Read` as facades of the same engine, deterministic router, external domains only through MCP, off by default with `KATANA_UIOE=off|pack|full`) | the greymod question gets the context block before the model starts and the model answers in fewer steps; gap analysis against the research done. **Design in progress** |
| T4 | Index life cycle: root by `katana init`, crawler on demand in levels (tree, text, symbols, links), `Symbols tree`, transparent ignored folders, unified `Find` index. Design: `docs/code-index-lifecycle.md`, nine phases in five waves | phases done one by one with their criteria. **Phase 1 in progress** |
| T5 | **Decision of 2026-11-23: the curriculum is updated in place (v7), not forked into a v8, and only at the END of the UIOE construction.** The v7 data in `data/curriculum/v7` is a snapshot of 11:04 of that day; the training started on it was stopped at step 25 (nothing saved). What to teach: the new actions and blocks (`Symbols flow`, `Symbols tree`, the `[katana: context ...]` block, index state lines, `called by:`/`calls:`/`links:` in `Find`, the lean tool list from `docs/tools-review.md`, `Symbols` in the central list). The data of 11:04 does not know them; regenerate every family with the final binary, then train once from scratch | v8 data generated with real tool outputs and `curriculum check` clean |
| T6 | The web page of `katana serve --agent` has only been checked against a mock server | it drives a real session with Qwen3.6 and the WIP mark is removed |

## Priority 1: Vulkan backend (closes the 11–16x gap on the prompt)

| Step | What | Done when |
|---|---|---|
| 1 | Batch with Q4_K (and Q6_K) in the katana layout, decoding at tile load (`tools/vkq4k`) | **done**: 1.96–2.69 T mult/s on Q4_K and 2.17–3.84 on Q6_K, verified. Still missing: a narrow tile for small batches |
| 2 | Runtime layer (`vk/kvk.h`) and port of the per-token kernels (`tools/vkmv`) | **done**: tests pass (runtime on both cards; kernels on NVIDIA). Throughput at the same level as OpenCL, still without an identical measurement. Missing: per-token batch, several experts per launch and fused kernels |
| 3 | Integration into `katana.c`: backend selection (`KATANA_BACKEND`), weight loading, per-token product | **done**: `selftest` (products) passes through Vulkan on 5 dense models; the graph still runs on the CPU in this backend. Not tested: MoE, Intel card, fallback to OpenCL when Vulkan fails |
| 4 | Full dense graph in Vulkan (norms, attention, final projection), generation | **done** (measured alternating): Qwen 1.5B 99.7 tok/s on both backends; SmolLM2 135M 409 tok/s on Vulkan against ~130 on OpenCL (reference Vulkan 223, CUDA 402). `selftest` passes on 5 models and the greedy text comes out identical to OpenCL's. Prompt still one token at a time (121 against 212 tok/s) |
| 5 | Batched prompt with tensor cores | **done**: Qwen 1.5B 1,387 tok/s on the 64+64 benchmark (was 212 on OpenCL; reference 2,343), Llama 3B 757, SmolLM2 8,100. Still missing: a faster product (today ~2.7 T) and cheaper attention to get close to the reference |
| 6 | MoE on Vulkan: expert cache, cold experts on the CPU | Qwen3-30B passes `selftest`; generation equal to or better than today's |
| 7 | Graph recorded and resubmitted per token | **done in step 4**: 409 tok/s on the 135 M model |

Pending decisions: Vulkan headers inside the repository or declared by hand; SPIR-V generated
at build time or versioned as a C header (versioned spares the shader compiler for anyone who only
builds the engine).

## Priority 2: DeepSeek/Kimi architecture

The "trillion-parameter model" target is from the Kimi family (MoE with low-rank attention). Kimi K2 is
~600 GB at 4 bits and does not fit on this machine; the code is validated on a small sibling.

| Step | What | Done when |
|---|---|---|
| 1 | Download DeepSeek-V2-Lite (16 B, ~10 GB) and read the metadata | `katana info` shows the architecture and the tensors |
| 2 | Low-rank attention on the CPU: two-stage q, compressed K and V shared across heads, RoPE on a slice, per-head decompression | greedy output matches the reference engine for dozens of tokens |
| 3 | Sigmoid router with bias, shared expert, initial dense layers | same |
| 4 | GPU path and expert cache | `selftest` passes; measure against the reference |
| 5 | Test with Kimi, if there is storage (external SSD ≥ 1 TB) | it runs, at whatever speed it gets |

## Priority 3: CPU for the cold experts

On Qwen3-30B almost half of the time per token is CPU.

- 8-bit activations with a scale per block of 256 and partial sums. Criterion: same token chosen
  as the 16-bit path over 1,000 tokens of real text, and faster.
- AVX-512 VNNI with run-time selection.
- 8 rows per step.

Estimated gain (not measured): up to 1.5–2x on what fits in cache.

## Priority 4: what makes the engine an assistant

- Exact pre-tokenizer by the type declared in the GGUF. Criterion: same tokenization as the reference
  engine on a set of texts (code, numbers, accents, line breaks).
- KV cache: attend only to the positions in use, prefix reuse, context shifting, context
  larger than 4,096.
- Sampling: repetition penalty, top-p, min-p.
- Context compaction: summarize old turns when the context fills up, and measure what is lost.
- Agent: JSON output guaranteed by grammar, more tools, configurable limits.

## Priority 5: network between machines

- Each machine holds part of the experts and answers for them; the one that asked sums the results.
- First experiment: two instances on the same machine exchanging experts over a socket, to
  measure the round-trip cost per layer.
- Depends on a second machine for the real test.

## Out of priority (with reasons)

- Fusing more kernels in OpenCL: no measured gain above 1 B.
- Uneven VRAM slots per layer: 1 percentage point below the ceiling.
- Using more than 92% of VRAM: it gets worse.
- Experts with fewer bits only in VRAM: changes the model's output; only as a measured experiment.

## How to record progress

When a step is finished: measured number (alternating when it is a comparison), update of the table in
`knowledge.md`, commit and push. A hypothesis that was not confirmed also goes into `knowledge.md`,
in the list of what gave no gain.

## H. Conventions as training (2026-11-21)

`katana curriculum` (`docs/curriculum.md`) generates conversations in which the assistant follows `KATANA.md`
(brief), skills (ability) and MCP cards. It has 15 families and is checked by `katana curriculum check`.
`tools/codebench/tasks/conv-*` (12 tasks) is the measurement.

| step | done when |
|---|---|
| 1. Switch the curriculum's block functions (`cu_sys_project`, `cu_sys_skills`, `cu_sys_mcp`, `cu_skill_result`, `cu_refusal`, `cu_done_note`) to the engine's own presentation once it is merged | `curriculum check` passes and the system prompt matches `katana agentsys` with the same files in place |
| 2. Decide how a 512-token window holds the ~1,400-token system prompt (`--seq`, or `--system blocks`) | the trained turns see the project, skill and card blocks |
| 3. One night with `specialize all --curriculum` | `conv-*` on the specialized model beats the base (alternated, `--repeat 3`); the rest of codebench does not lose |

