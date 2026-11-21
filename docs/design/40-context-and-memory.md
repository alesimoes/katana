# Context and memory

What stays in front of the model, what is dropped, what it costs to drop it, and what survives a session.

## Where katana is today

- **Prefix cache** (`src/agent/prefix_cache.c`): the KV state of the system prompt is stored on disk in two levels
  (rules and tools, then the per-folder environment block); for Qwen3.6 on the GPU the file also holds the recurrent
  state of the 30 linear layers. Measured: a known folder loads in 0.05 s; a new folder computes about 940 tokens.
- **Compaction** (`agent_compact`, `cp_trig`, `ah_summary_live` in `src/agent/context.c`): triggered when the
  context passes system prompt + 6,000 tokens. The model writes a summary of up to 600 tokens from the live state;
  the history becomes summary, the user's request, the last call and its result, and that history is recomputed.
  The next compaction waits for 2,000 tokens of growth. The 6,000 is a constant chosen for speed and never measured
  against task results.
- **Result shrinking** (`ah_shrink_tool`, `ah_shrink_call`): old results and large call bodies are cut when the
  history is rebuilt.
- **Reading notes** (`tool_digest`): see `20-tools.md`.
- **Sub-agent**: saves the main KV state to disk, runs from position 0, reloads (`src/agent/delegate.c`).
- **Sessions** (`sess_save` in `src/agent/loop.c`): compact history, token ids, expert heat, full text.
- **Persistent memory**: `remember`/`recall` of the first tool set and a memory file injected in the environment
  block.

Consequence seen in the reference session: about 86,000 tokens of conversation passed through a window that keeps
6,000. Six files were read 24 times and the same commands were repeated up to three times. The model is not
stubborn; it has amnesia.

Facts about the two models that the design uses (model cards [25][27], and `docs/handoff.md`):

| | Qwen3.6-35B-A3B | Gemma 4 26B-A4B |
|---|---|---|
| layers with a growing KV cache | 10 of 40 (2 KV heads) | 5 of 30 global |
| other layers | 30 linear layers with a fixed recurrent state, 64 MB in fp32 (measured) | 25 with a ring of the last 1,040 positions |
| KV cost | 20 KB per token in fp16 (measured) | 2.9 GB at 131,072 tokens (measured) |

In both models the state that depends on *all* earlier tokens is small and fixed in size, and the growing part can
be truncated by moving a position. That is what makes snapshots cheap here (C4).

## What the frontier does

- **Deployed.** A stable, append-only prefix is treated as the main production metric by agent builders, because
  cached tokens are an order of magnitude cheaper [12]. Hosted agents clear stale tool results in place (a vendor
  reports 84% fewer tokens on a 100-turn evaluation) [14], summarise when the window fills, and keep long work in
  files the harness owns [65]. A local reference engine saves and restores slots of state; hybrid models needed
  fixes there because the recurrent state must be saved too [56].
- **Published.** Replacing old tool outputs with a placeholder, keeping the last 10 turns, costs about half as much
  as leaving them and solves as many tasks as model-written summaries; summaries lengthened trajectories by 13 to
  15% and cost up to 7% of the run [13]. Rule-based elision before any summarisation was the most efficient
  context policy in a controlled study, the elided content was rarely asked for again, and context management
  matters more as the window shrinks [78]. Model-written summaries are weakest at tracking files [15]. Models
  degrade with input length even on easy tasks [16]. A key-value prefix trained offline on a corpus matched having
  the corpus in context at 38 times less memory [44].
- **Unsolved.** Nobody reports the behaviour of small local models under compaction. Every hosted technique pays a
  re-read when it edits the middle of the context; with cheap cached tokens that is tolerable, and at 15 ms per
  token it is not.

## Design

### Now

**C1. Measure the depth curve, then set the trigger.**
- What: generation and prompt speed at 4k, 8k, 16k, 32k and 64k tokens of context, both models, GPU path
  (`katana bench` at depth, alternating); then the compaction trigger becomes the depth at which generation speed
  has lost 15%, not a constant.
- Mechanism: the trigger trades speed at depth against amnesia. The speed side has never been measured on the GPU
  path (plan item C5); the quality side is experiment 3 of `90-roadmap.md`.
- Cost: no code for the measurement; one line in `cp_trig` for the result. Drop: nothing to drop; if speed falls
  fast, the trigger stays low and C4 becomes more urgent.

**C2. The engine ledger replaces the model-written summary.**
- What: the engine keeps, per session, a structured record updated by the dispatcher (no model call), rendered in
  a fixed format when the history is rebuilt:

  ```
  [ledger]
  request: <the user's message, first 300 bytes>
  later: <each later user message, 200 bytes>
  plan: 1x 2x 3> 4. 5.
  files: src/p.c +12 -3 | tests/t.py new | Makefile read
  ok: make test (14 pass) | cc -O2 -o x x.c
  failed: xtool auth x3 (denied) | pip install foo (not-found)
  found: <step notes written by the model, one line each, 100 bytes>
  ```

  Sources: paths and line counts from `Write`/`Edit`, commands with exit codes and fault kinds from `Bash`
  (`10-agent-loop.md`, L1), the task list from `todo.c`, user messages from the history. `found:` is the only
  model-written part: an optional `note` on a plan update (L6).
- Mechanism: P1. The summary today costs up to 600 generated tokens (24 s) per compaction and is weakest exactly
  where the engine is exact (which files, which commands, what failed) [15]. A fixed format can be taught by the
  conventions curriculum and costs about 150 to 300 tokens to read.
- Evidence: [13][15][78]. Cost: `src/agent/ledger.c` (about 300 lines), hooks in `agent_tool` and `todo.c`,
  `agent_compact` renders it instead of calling `ah_summary_live` (kept behind `KATANA_COMPACT=summary`). One week.
- Measure: experiment 3 (`90-roadmap.md`). Drop if tasks solved fall against the summary on long tasks.

**C3. Rebuild with masking, not with a summary.**
- What: at compaction the history becomes: ledger, every call kept as written, results older than the last 8
  rounds replaced by `[r12: 212 lines, see ledger]` (the handle of T3 in `20-tools.md`), the last 8 rounds verbatim.
- Mechanism: the model keeps the shape of what it did (which stops repeats) and loses only bulk it can fetch
  back.
- Evidence: [13] (window of 10 turns). Cost: `ah_shrink_tool` already cuts results; the rule changes from "cut to
  N bytes" to "replace with the handle line". Two days after T3 exists.
- Measure: with C2, in experiment 3. Recomputation per compaction (tokens re-fed) is recorded in `KSX2`.

**C4a. A third level in the prefix cache.** System prompt, environment block, then project file plus ledger of the
last session: a session resumed in a known project starts from disk. Cost: one more key in `pfx_prepare`. Measure:
seconds to first token on resume.

### Next

**C4. State snapshots and scopes (original).**
- What: at every round boundary the engine can take a snapshot: the position, and a copy of the state that does
  not truncate (Qwen3.6: the 64 MB recurrent state and the convolution rings; Gemma 4: the rings of the 25 local
  layers). The growing KV needs no copy: rolling back is setting the position. A **scope** is a snapshot with a
  name. Closing a scope restores the snapshot and feeds a short replacement text in place of everything the scope
  contained.

  Three uses:
  1. *A plan step is a scope.* When step k is marked done, the engine rolls back to the start of step k and feeds
     one ledger line for it (what changed, what ran, the model's note). The context is then: prompt, request,
     plan, one line per finished step, the current step verbatim. Old work leaves the context at the cost of
     feeding 30 to 80 tokens: no summary generation, no recomputation.
  2. *Exploration is a scope* (`10-agent-loop.md`, L9): the sub-agent without a prompt re-read and without a disk
     round trip.
  3. *Speculation is a scope* (L12).
- Mechanism: P6. Every hosted technique that removes something from the middle of a context pays to re-read
  everything after it. Cutting a *suffix* is free when the state before it can be restored, and plan steps make the
  suffix exactly the part that is finished. In both reference models the non-truncatable state is small and fixed
  in size.
- Evidence: none for scopes by state rollback in a shipped agent (landscape report, topic 4). The components are
  supported: elision works as well as summaries [13][78]; elided content is rarely needed again [78].
- Cost: `snap_take`/`snap_restore` beside `pfx_ssm_io` in `prefix_cache.c` (the state is already serialised
  there for the disk cache; the snapshot is the same data kept in RAM, 64 MB each for Qwen3.6, a ring of 32 is
  2 GB); the Gemma 4 ring copy; scope bookkeeping in `agent()`; the task list calls `scope_close` on a completed
  step. For the GPU path the state lives in VRAM: a download per snapshot, an upload per restore. Two to three
  weeks. Must pass: greedy output after restore identical to the output of a context built by feeding the same
  text from scratch.
- Risks: a later step needs something a finished step read. Mitigations: the step note, T4 (the file is read again
  on demand, now a legitimate read), handles stay valid. If the model marks steps done too early, the scope closes
  on unfinished work: the ledger line still says what was changed.
- Measure: snapshot and restore milliseconds on both models (GPU path); then experiment 3 with a third arm: tokens
  recomputed per task, rounds, solved tasks, re-read rate.
- Drop: if restore costs more than re-feeding 500 tokens (7.5 s), or if solved tasks fall against C2+C3.
- Confidence: medium to high on the mechanism (the state layout is known and already serialised), medium on the
  behaviour.

**C5. Failed exchanges leave the context once they are resolved.** After a failed call is followed by a successful
call of the same tool on the same target, the failed exchange is dropped at the next rebuild (or, with C4,
immediately: roll back to before the failure and feed the successful call and its result, about 300 tokens at
reading speed). The failure stays visible while it is useful, as the evidence recommends [83], and stops costing
attention afterwards. The pair is also the training example of T3 in `60-training.md`. Measure: context tokens per
solved task; repeat rate. Drop if repeats of the failed call rise (the model forgot it failed).

### Beyond

**C6. A trained state per project (original for this architecture).**
- What: a per-project file holding a trained prefix state (for Qwen3.6: an initial recurrent state for the 30
  linear layers plus the KV of a few hundred virtual positions in the 10 attention layers), optimised offline so
  that the model answers the project's question-answer corpus as if the notes were in context. Loaded like the
  prefix cache, at no reading cost.
- Mechanism: facts enter through the state instead of the weights. The base behaviour cannot be damaged (no weight
  changes), a stale project state is deleted and retrained without touching anything else, and states of different
  projects do not interfere. It is trained, not retrieved, so it stays within the owner's decision that knowledge is
  learned, not looked up.
- Evidence: [44] on standard attention models: equal to in-context at 38 times less memory, and plain next-token
  training of the prefix was not enough (the corpus must be synthetic questions and answers, which
  `katana knowledge corpus` already produces). Nothing published for hybrid recurrent layers or 4-bit weights.
- Cost: the trainer needs gradients with respect to the initial state and the prefix KV (the backward pass exists
  in `src/app/train.c`; the parameters are new), at the same 245 s per window. Three weeks.
- Measure: the held-out questions of the knowledge corpus (plan item G3), against a rank-8 adapter trained on the
  same windows, and against the notes simply placed in context (the ceiling). Drop if recall is under half of the
  in-context ceiling after three nights.
- Confidence: low to medium.

**C7. Context that carries its own cost.** Each message in the history gets a cost and a last-use round; the
rebuild keeps the highest value per token under a budget instead of a fixed window. Needs C2 and `KSX2` data to
define value (was this result referenced by a later call?). Confidence: low; listed so that the data is collected.

**Rejected:**
- Precomputed KV states for common tool-result prefixes. The state at a position depends on every earlier token, in
  the attention layers and in the recurrent layers, so a result's state cannot be computed once and placed after
  different histories. Only whole prefixes can be reused; C4 and C4a are that idea in its valid form.
- Model-written reading notes as the default (`tool_digest`): generation is 2.6 times dearer than reading.
- Retrieval at run time from the knowledge notes (owner's decision; also a second index to maintain).
- Rendering context as images to cut tokens: independent studies found no gain over truncation at equal budgets
  [63].
- Prompt compression by a second model: needs a second model in memory, and the evidence on tool-using agents is
  negative or missing [53].
