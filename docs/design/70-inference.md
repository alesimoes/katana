# Inference

The engine's part of the token economy: making a token cheaper, and making fewer of them need a full generation
step. Engine measurements are in `docs/handoff.md` (0i, 0j, 0k) and `docs/comparisons.md`; this document does not
repeat them.

## Where katana is today

- Qwen3.6-35B-A3B on a 4 GB card: generation 24 to 27 tokens/s, prompt 60 to 68 tokens/s (measured, machine
  free). Dense weights and attention in VRAM, 17 expert slots per layer of 256 experts, hit rate 36 to 47% on agent
  prompts, cold experts on the CPU at about 22 ms per token when all are cold (bandwidth ceiling about 17 ms).
- Measured dead ends: unequal slots per layer, a preloaded heat file from another session (slightly worse than
  none), the router on the GPU, dense weights moved to the CPU to open slots, lower swap margins (uploads block the
  token). The best fixed expert set reaches the same hit rate as the current policy; the ideal eviction policy
  would reach 61 to 67%.
- A lookup-based speculative decoder exists for `katana chat`; the agent's sampling loop (`gen_console`,
  `src/app/console.c`) does not use it.
- Open items of the plan: CPU experts overlapping GPU work inside a layer (C1), one more slot (C2), wider CPU
  kernels (C3), Gemma 4 start time (C4), long context on the GPU for Qwen3.6 (C5), all "after measuring where the
  time of one token goes", which has not been done.

## What the frontier does

- **Deployed.** The common reference engine places experts of the first N layers in RAM and the rest in VRAM, and
  since late 2026 has a dynamic expert cache; reports on cards of 8 to 12 GB give 20 to 40 tokens/s for models of
  this class at lower-bit quantizations, a hand-tuned static split is within 1 to 5% of the dynamic cache in one
  test, and thread count matters by an order of magnitude in another [56][57][58]. Key-value caches at 8 bits are
  reported near lossless; 4 bits degrade [62]. Both reference families publish draft heads for multi-token
  prediction; reports for mixture-of-experts models at batch size one are mixed, and one vendor warns the speedup
  may not appear because each drafted token can route to different experts [50].
- **Published.** Predicting the next layer's experts from the current hidden state: about 90% hit rate past the
  first layers and 9 to 14% less time per token on a large card over a fast bus [59]. Pruning experts by
  router-weighted activation: about 2% mean loss on coding evaluations with a quarter to half removed, not shown
  on agent tasks at this size [60]. Drafting from a suffix index of the prompt and earlier outputs: 2.5 times
  faster on an agentic coding benchmark, 1.7 times better than plain prompt lookup, in a batched serving setting
  [49]. Cutting reasoning short by sampler rules: 18 to 50% fewer reasoning tokens, accuracy cost on agents
  unmeasured [51]. Text diffusion variants of one reference family generate an order of magnitude faster on large
  cards and need about 18 GB of VRAM [61].
- **Unsolved.** No published prompt-speed figure for a 35B mixture-of-experts model on a 4 GB card. Quantizing the
  recurrent state of the hybrid layers. Whether any draft-and-verify scheme pays at batch size one on a model whose
  active set changes per token.

**For this machine:** generation speed is within reach of the published numbers for larger cards, and the measured
ceilings say the remaining headroom in the expert cache is modest. Results are 63% of the tokens, so **prompt speed
is the term with the most leverage and the least attention so far**; and the cheapest generated token is the one
that is copied or forced instead of sampled.

## Design

### Now

**I1. Split the time of a generated token and of a prompt batch.**
- What: with `KATANA_TIME=1`, per token: dense products on the GPU, expert products on the GPU, expert products on
  the CPU, uploads, waiting, sampling; per prompt batch of 256: the same plus attention. Three alternating runs,
  both models.
- Mechanism: the plan's block C is gated on this number. For the prompt nobody has looked: an estimate from the
  measured unit costs (a 256-token batch touches every expert of every layer; about 82,000 expert products on the
  CPU) says the prompt is bound by CPU expert arithmetic, which would make plan item C3 (wider kernels, several
  rows per call) worth more for reading than for writing.
- Cost: timers exist in parts; one day. Drop: n/a.

**I2. Copy drafts in the agent's sampling loop.**
- What: when the text being generated matches a span of the context for 8 or more tokens (the usual cases: the
  `old_string` of an edit, a file rewritten with small changes, a path, a command repeated with one change), the
  engine drafts the next 16 to 64 tokens from the source and verifies them in one batched pass; accepted tokens
  cost reading time. With the state-grounded constraint on `old_string` (`30-call-formats.md`, F5) the source is
  known exactly and acceptance should be near total.
- Mechanism: a verified batch runs at prompt speed (60 to 68 tokens/s) against 24 to 27 sampled. In the reference
  session `Edit` and `Write` bodies were 10,500 written tokens; the copyable share is an estimate of one third to
  one half.
- Evidence: [49]; the engine's own lookup decoder in `chat`. Risk specific to this architecture: a rejected draft
  must restore the recurrent state (the snapshot of C4 in `40-context-and-memory.md`, or verifying before
  committing the state).
- Cost: connect the existing decoder to `gen_console`, with a suffix index over the session text instead of a
  linear scan. One week, more if the state restore is not there yet.
- Measure: accepted tokens per session, seconds per `Edit` call on the edit micro-benchmark, alternating on and
  off. Drop if seconds per call do not fall by 20% on edits.

**I3. Forced tokens** (`30-call-formats.md`, F3) and **the reasoning cap** (`10-agent-loop.md`, L4) are sampler
features and are listed here for completeness.

**I4. Thread counts, measured again on the agent's real mix.** 12 threads beat 8 in the last measurement and 16
was worse for the experts; a published case lost 8 times by using all threads [57]. One alternating run of
generation and of prompt at 6, 8, 10, 12 with the desktop and a training run present, since that is the machine's
real state. Separate counts for generation and prompt if they differ (the optimization list already suggests it).

**I5. The plan's C1 (CPU experts while the GPU works in the same layer).** Unchanged; it is the largest known
item for generation. Measure as planned.

### Next

**I6. Token-local expert prediction, tested in the simulator before any engine work.**
- What: two predictors evaluated offline on recorded routing logs (`KATANA_EXPLOG`, `tools/sim_vagas.c`):
  (a) a co-occurrence table from the experts chosen at layer l to those at layers l+1 and l+2; (b) a table keyed
  by the phase the call grammar is in (prose, call framing, a code value, a path). The simulator reports the hit
  rate if predicted experts were made hot one layer or one token ahead.
- Mechanism: the measurements rule out slow signals (session heat, calibration sets) and leave 20 points between
  the current policy and the ideal one. Whatever closes that gap is local to the token. Published work reaches
  about 90% with the hidden state as input [59]; a table costs nothing to try first.
- Cost: simulator work only, two days; logs from three agent sessions. Then, if it passes: a prefetch of the
  predicted cold experts into CPU cache during the GPU's attention work, and non-blocking uploads (already the next
  step of handoff 0j).
- Gate: simulated hit rate at least 8 points above the current policy at the same number of uploads. Drop below
  that: each point of hit rate has been worth little (margin 8 gave +3% generation).
- Confidence: low to medium.

**I7. Long context on the GPU path, with the depth curve** (`40-context-and-memory.md`, C1; plan item C5) and the
8-bit key-value cache measurement that is already specified in handoff 0e.

**I8. A smaller file for the minimum machine.** The catalog's minimum is 24 GB of RAM and the reference file is
22 GB. Removing the least-used quarter of the experts by router-weighted activation, calibrated on the owner's
agent sessions, is published as near lossless on coding evaluations [60] and would bring the file to about 17 GB.
It changes the model's output, so it is an experiment with a gate: the codebench micro-set and decision agreement
with the unpruned model (`80-evaluation.md`, E1). Not for the development machine, which has the memory. Drop if
the micro-set loses more than one task in two runs or agreement falls under 95%.

**I9. Draft heads.** Both model families ship multi-token prediction heads [25][50]; katana ignores the extra
layer. One experiment after I2, because both need the same verify-and-restore path: accepted tokens per step and
net tokens per second at batch size one. Expectation from the published reports: no gain on this hardware. Drop at
under 15% net.

### Beyond

**I10. Two precisions by temperature.** Hot experts exact in VRAM; a second, lower-bit copy of the cold experts in
RAM, used only when an expert is cold. The cold path is bandwidth-bound (measured), so fewer bits is the only way
under its ceiling. It changes the output whenever a cold expert is used, which the project has so far allowed only
as a measured experiment. Gate: decision agreement and the micro-set. Confidence: low; the published support was
not verified (landscape report, topic 7).

**I11. Speculation in the gaps.** While a tool runs or the user types, the engine is idle. Uses, in order of
likely value: generating the next turn under the passing verdict (L12 of `10-agent-loop.md`); taking the snapshot
and building the suffix index; pre-running the project's test command once so its duration is known (V1). None
generates tokens the user did not ask for unless they are thrown away for free.

**Watch, do not build:** diffusion text models of the Gemma family (need about 18 GB of VRAM today [61]);
compressed-attention designs of the newest very large open models [28] (far above this machine).

**Rejected:** a second small draft model (memory and a second architecture to maintain; the copy drafts cover the
high-acceptance cases); early exit by layer skipping (no evidence for these hybrids; changes outputs); another
round of slot-policy tuning (the fixed-set and decay variants are measured and flat).
