# katana: plan

Written on 2026-11-15. Each step has a measurable done criterion. Context is in
`knowledge.md`; techniques are in `optimization-roadmap.md`.

## Current goal (2026-11-21): an independent local agent, pleasant and stable to use, that other applications can use as their agent

Set by the owner on 2026-11-21; it replaces the order of the priorities below, which are from 2026-11-15 and mostly
about dense models. Only Qwen3.6-35B-A3B (and its AgentWorld variant) and Gemma 4 26B-A4B count. Each line is done
when its criterion is measured; "not measured" items stay open even when the code exists.

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

**B. The model understands the request better**

| # | What | Done when |
|---|---|---|
| B1 | Reasoning on or off in the desktop (it starts with `KATANA_THINK=0`) | codebench and a set of ambiguous requests in Portuguese, alternating, both models |
| B2 | Reading notes (`KATANA_DIGEST`, on by default and never measured): a file read becomes a 250-word note | codebench with and without, alternating; default follows the result |
| B3 | Qwen3.6 against AgentWorld against Gemma 4 as the desktop default | same tasks, same hour |
| B4 | First set against second set of tools | handoff 0t |
| B5 | Harder tasks (the terminal and web ones are solved by every model) and requests written the way the owner writes | tasks that separate the models. **Tasks written (2026-11-21), not yet run with any model:** 12 tasks that need new code: `write-*` (6, from an empty folder), `prereq-*` (3, the code needs a command, a credentials file or a service that is not on the machine), `extend-*` (3, a new module plus changes in existing files of a 300 to 400 line project); 8 in informal Portuguese, 4 in English. They pass `run.py --verify` (fail untouched, pass with the reference solution, no model). Open: run them on Qwen3.6 and Gemma 4, see which separate the models, and read the first runs for ambiguous hidden tests |

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

**E. Install and run anywhere**: AppImage and the packaging workflow executed once; Windows build compiled again;
`test_server.py` and a real client against the two models; native `tools` in the OpenAI-compatible API (**code done
2026-11-21**, `docs/api.md`: native formats, typed arguments, streaming deltas, Gemma 4 reasoning split; tested on
tiny models with a fixed raw output, not with Qwen3.6 or Gemma 4).

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

**G. Knowledge from earlier sessions** (the owner wants the model, not a lookup, to know what was done in his projects)

| # | What | Done when |
|---|---|---|
| G1 | `katana knowledge build/search/stats/corpus` and `docs/knowledge-base.md` (2026-11-21) | **done**: notes and corpus built from the real transcripts, tested without a model |
| G2 | Train a model on the corpus (`katana specialize <model> knowledge`) | the adapter trained, nights counted |
| G3 | Held-out measurement: ask the `--holdout` questions before and after training | correct answers counted for both; the expectation (names, paths, commands, not volume) confirmed or refuted |
| G4 | Keep the base current: rebuild incrementally, regenerate the corpus, retrain on the new part | a routine the owner can run |


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
