# Training

What goes into the weights, with which data, on which parameters, and how a night is spent. The budget is the
design problem: 245 s per 512-token window on the CPU (measured), 100 to 150 windows per night, 50 to 75 thousand
tokens seen, about 300 of every 512 being targets (31,927 targets in 106 windows, measured on the first run).

## Where katana is today

- A complete training engine in C: `katana train` (`src/app/train.c`: hand-written backward for both reference
  architectures, AdamW, low-rank adapters on a quantized frozen base, loss on marked spans), `katana import`,
  `export`, `learn`, `merge`, `specialize` (five sources with an explicit mix, resume across nights with
  `history.jsonl`), `katana knowledge corpus` (3,000 question-answer pairs from past sessions), and a conventions
  curriculum generator in progress.
- The standing recipe: `--train 'blk.*.attn_*' --rank 8 --seq 512 --lr 2e-4 --val 0 --final 0`. On Qwen3.6 that
  is 100 adapters (the projections of the 10 attention layers and of the 30 linear layers) and 60 norm tensors,
  5.74M weights (measured plan of the first run). No feed-forward or expert weights are trained.
- Measured on a real model: cost only. One run of 48 steps (training loss 2.04 to 1.57 on its own data, no held-out
  set), discarded. **No specialization has ever been evaluated on a task.**
- A specialization that changes experts runs on the CPU only; adapters are merged at load into float copies of the
  adapted matrices; `katana merge` writes a new GGUF re-encoding changed tensors in their original quantization.

## What the frontier does

- **Deployed, with public results.** Distilling a strong agent's trajectories into an open 32B model: 491
  trajectories gave +13.6 points on a public benchmark and had not plateaued [31]; 5,016 gave +33 points [30];
  200,000 soft-verified synthetic ones gave a 32B model 54% without any tests in the data pipeline [32]. All are
  full fine-tuning runs on dense models. Reinforcement learning with test rewards reached similar levels at about
  384 accelerator-days for one model [33]. Tool-call formats are taught with hundreds of examples: 500 examples
  moved a 1B model from 1% to 93% function-name accuracy in a vendor tutorial [71].
- **Published.** A study of low-rank adaptation across model families, including a 30B mixture-of-experts model of
  the Qwen family: adapters on attention only clearly underperform adapters on the feed-forward weights, and adding
  attention to feed-forward gives nothing more; the best learning rate is about 10 times the full fine-tuning rate
  (higher for runs under 100 steps) and hardly depends on rank; larger batches hurt; for the mixture-of-experts
  model the adapters were per expert, and the right configuration is called an open question [34]. Adapting only
  the 25% most-routed experts per layer stays within a point of adapting all [35]. Unfreezing the router helped in
  one study [36] and needed anchoring to the base router in another [37]. Low-rank adaptation learns less and
  forgets less than full fine-tuning, and is clearly worse at absorbing new material [41].
- **Facts.** Every positive result on putting documents into weights used orders of magnitude more data than a
  night here: 455 million synthetic tokens for 1.3 million real ones [38], billions of words [39]. Plain paraphrase
  saturates early [38][39]. New facts are learned slowly and, once learned, raise hallucination elsewhere [40].
  Questions asked in the reverse direction fail unless the reverse form was trained [42]. A hypernetwork that
  emits an adapter from a document exists for 2B-class bases only [45]. Merging an adapter into a 4-bit base and
  re-quantizing is reported lossy [47].
- **Unsolved.** Learning curves under 1,000 trajectories for a mixture-of-experts model. Facts in a 35B model from
  thousands, not millions, of examples: **no evidence either way**. Replay ratios for nightly sequential
  adaptation.

**For this budget:** habits are supported by evidence at hundreds of examples; trajectories need months; facts as
question-answer pairs are unsupported. Design accordingly, and let two experiments (`90-roadmap.md`) confirm or
refute before more nights are spent.

| goal | least data reported to work | at 50 to 75 thousand tokens a night |
|---|---|---|
| a call format, a closing habit, a plan habit | hundreds of short examples [71] | 1 to 3 nights (estimate) |
| agent skill from trajectories | 491 full trajectories, full fine-tuning [31] | 35 to 100 nights (estimate) |
| project facts as questions and answers | not established | unknown; experiment 2b |
| reinforcement learning with tests | hundreds of accelerator-days [33] | no |

## Design

### Now

**R1. One behaviour per night, with a probe before and after.**
- What: `katana specialize` takes a *target* instead of a five-way mix: one behaviour, its data family, its probe.
  Suggested order: (1) valid calls without corrections, (2) a text answer at the end of every request, (3) several
  independent calls in one turn, (4) plan before the first change, (5) reading the ledger format. Each night: 80%
  target family, 20% replay drawn evenly from all earlier nights' data. The probe is the offline replay of
  `80-evaluation.md` (E1): minutes, no generation.
- Mechanism: P7. Five sources at 20 to 30 windows each are all below any published threshold; one target at 100
  windows is at the edge of one.
- Cost: `--target NAME` in `src/app/specialize.c` (selects the import options and the probe); `--mix` stays for
  replay. Two days. Measure: the probe metric of the target, before and after. Drop the target approach if no
  target moves its probe in two nights each (then nothing moves at this budget and R6/R7 come first).

**R2. Loss on decisions.**
- What: `katana import --train-decisions [N]`: in each model turn the targets are the text, and for each call the
  tool name, the parameter names and the first N tokens (default 48) of each value; the rest of a long value (file
  bodies, long commands) stays as context. Training text is rendered with the **run-time result shaping** (T2,
  T3 of `20-tools.md`): results clipped and replaced by handle lines, so more turns fit in a window and the model
  trains on what it will see.
- Mechanism: a window costs the same whatever it holds. A `Write` body of 650 tokens is one decision; the same
  window holding six short turns is six. Estimate: 3 to 5 times more decisions per night.
- Evidence: loss masking to assistant spans is standard; masking inside the call is this project's inference from
  the budget (no published measurement found).
- Cost: span computation in `src/app/importer.c` (the call readers already locate values). Three days.
- Measure: decisions per window (printed by `import`); probe gain per night against whole-turn spans, same data.
  Drop if the probe moves less than with whole turns.

**R3. Never a night without a held-out number.**
- What: a fixed validation file of 20 windows per target, forward only, evaluated once at the end of the night and
  recorded in `history.jsonl`; plus the probe. `--val 0 --final 0` together is refused by `specialize`.
- Cost: forward-only evaluation is a fraction of a step; 20 windows is about half an hour (estimate from the
  measured step time; measure it). Drop: never.

**R4. Nights that really continue.**
- What: the AdamW moments and step count are saved beside the `.kspec` (the trainer state was 88 MB in the first
  run) and restored by `--resume`; the schedule is one warm-up on the first night, then constant.
- Mechanism: today each night restarts the optimizer and runs a warm-up and a cosine decay to 10% inside 100 to
  150 steps: a sawtooth in which a third of every night is spent at a low rate.
- Cost: one sidecar file in `train.c`. One day. Measure: training loss at step 20 of night 2 against the end of
  night 1 (continuity).

**R5. Adapters where the evidence says they work.**
- What: add the always-active feed-forward path to the trained set: the gated shared expert of each layer in
  Qwen3.6 and the dense feed-forward branch in Gemma 4, at rank 16, with the attention adapters kept. Router
  frozen. Routed experts untouched for now (R9).
- Mechanism: [34] found attention-only adaptation the weakest placement, on a model of this family. The shared
  expert is feed-forward capacity that every token uses, and it is a dense tensor: it does not go through the GPU
  expert cache, so serving stays on the GPU.
- Cost: a pattern for `--train` (the trainer already has gradients for shared experts; `gradcheck` covers them on
  the tiny models). Check `--plan 1` for memory. One day plus the comparison.
- Measure: two half-nights on the same windows (attention only; attention plus shared feed-forward), held-out
  decision loss and the probe. Drop the addition if the held-out loss is not lower.

**R6. Check what a specialization costs at serving time before using one.**
- What: three alternating benchmarks on the real model: base; base + `.kspec` (adapters merged at load as float
  matrices); merged GGUF from `katana merge`. Record tokens per second, expert slots that fit in VRAM, and the
  held-out decision loss of each.
- Mechanism: two unmeasured risks. Float copies of adapted matrices are several times larger than their 4-bit
  form; if they go to VRAM they displace expert slots (each slot is about 1.2 points of hit rate, measured). And
  re-encoding `W + BA` into the base's 4-bit format can round away a small low-rank update [47]; on the tiny models
  the merge was only checked to answer the same three sentences.
- Cost: no code. If either risk is real: apply adapters unmerged in the products (`W x + s B (A x)`, already item 6
  of the specialization plan), which also enables swapping adapters without a reload.
- Drop: n/a; it gates plan item D4.

**R7. Measure training on the GPU (plan item D5).** One run of `tools/traintest/run.sh` on the real card and four
steps with and without `KATANA_TRAIN_GPU=0`. If the GPU is 3 times faster, every budget in this document triples.
Half an hour of machine time decides the largest multiplier available.

**R8. Data rules.**
- Reasoning: 96% of the reasoning blocks in the imported transcripts are empty. Train with reasoning off (the
  empty block as the template writes it) and serve the same way, until L4 (`10-agent-loop.md`) defines when the
  model reasons; then render training data with the same policy.
- Plans derived in hindsight (`--plan-first`) contain the detours of the real work. Derive the plan only from
  steps whose calls succeeded and were not undone later, 3 to 6 steps.
- Cap commands on this repository to 20% of the windows (they are 80% of the transcript calls).
- A specialization is tied to one base file; choose the base first (experiment 1).

### Next

**R9. The grammar as a teacher (original).**
- What: every time the call grammar (`30-call-formats.md`) rejects the sampled token and resamples, the engine
  records the context, the rejected token and the accepted one. Every state-grounded constraint that fires (F5)
  does the same. These records are training examples whose label is certain: at this position, this token, not
  that one. A night's target family can be built from them.
- Mechanism: constrained decoding fixes the output and leaves the model's distribution wrong, so the constraint
  fires forever, and a masked sample can be valid but unnatural. Training on the corrections moves the
  distribution to where the grammar already is. The number of corrections per 100 calls is both the curriculum and
  the metric, and it falls to zero when the lesson is learned.
- Evidence: none found for training on decoding corrections; the pieces (constrained decoding raising schema
  validity from 92 to 100% [21]; formats learned from hundreds of examples [71]) are established.
- Cost: a log line in `cg_check` (context position and two token ids; the session file supplies the text), an
  importer mode that turns records into single-token spans. One week.
- Measure: corrections per 100 calls before and after a night. Drop if the rate does not fall by a third.
- Confidence: medium to high.

**R10. Failure-driven curriculum with hindsight (original as a nightly loop).**
- What: from each day's sessions the engine extracts every failed call that was followed by a successful call of
  the same tool on the same target, and writes a training conversation in which the failed exchange is removed and
  the successful call is the target at the point where the failed one was made. Grouped by fault kind (L1); the
  night's target is the kind with the most failures that day.
- Mechanism: the curriculum is what the model got wrong today, corrected by what it did right a round later. No
  teacher, no tests, no judge. It is the correction data that imitation learning needs and that recorded teacher
  sessions lack (a teacher rarely fails in the model's ways).
- Evidence: learning from one's own corrected mistakes is the idea behind interactive imitation learning; no
  published nightly loop on agent logs was found. Soft-verified data is good enough for supervised training [32].
- Cost: `katana import --hindsight` over `.ksess` files (pairing logic shared with C5 of
  `40-context-and-memory.md`). One week. Measure: failed-result rate per fault kind in the following days'
  sessions and on the micro-set. Drop a kind after two nights without movement.
- Confidence: medium.

**R11. Operational facts: facts in the form in which they are used.**
- What: the knowledge corpus is rendered, in addition to questions and answers, as *decisions*: the environment
  block of the project (folder, map), a request in the owner's words, and as target the first call that the facts
  imply (in this project "build it" is `Bash` with this command; the tests are run this way; this file is where
  that feature lives, so the first call is `Read` of it).
- Mechanism: the agent rarely needs to *state* a fact; it needs to *act* on it. A fact rendered as the first call
  in its project is a habit keyed on the environment block, which is the kind of thing the budget can move (R1),
  and it is measured by the replay probe instead of by string matching of answers.
- Evidence: none specific. It follows from [40] (facts are learned slowly, behaviour quickly) and [42] (a fact is
  only available in the direction it was trained).
- Cost: a second renderer in `src/app/knowledge.c` using the commands and files already in the notes. Four days.
- Measure: experiment 2b in `90-roadmap.md`. Drop if first-call agreement on held-out requests does not rise.
- Confidence: medium.

**R12. More steps per night by training only the top layers.**
- What: adapters in the last K layers only (start with K = 12 of 40). The activations entering layer L-K depend
  only on frozen weights: cache them on disk per window (about 4 MB each) the first time, and later epochs start
  the forward pass there; the backward pass stops there always.
- Mechanism: step cost falls to roughly K/L for repeated windows and to (L + K)/2L for new ones. Estimate: 1.5 to
  3 times more steps per night. The quality cost of top-layer-only adaptation at this scale is unknown.
- Cost: a layer range in the trainer and a cache keyed by window hash. One week. Measure: seconds per step, and
  the probe gain per night against all-layer adapters. Drop if the gain per night is lower despite more steps.
- Confidence: medium on speed, low on quality.

**R13. Tasks generated from the owner's projects.** Mechanical mutations of code that has tests (an operator
flipped, a branch removed) produce tasks with a verdict; `katana learn` collects the model's passing attempts.
Published task synthesis works this way at scale [30][68]. The cost here is collection time (minutes per attempt),
so it runs when the machine is idle in the day, not instead of a training night. Measure: passing attempts per
hour. Drop under five an hour.

### Beyond

**R14. A specialization per project, on top of the general one.** A small adapter (or the trained state of C6 in
`40-context-and-memory.md`) per repository, loaded by working folder, holding only that project's operational
facts. It can be deleted and retrained when the project changes, which a merged general model cannot do. Requires
unmerged adapters (R6). Decided by experiment 2b: if facts do not go in at this budget in their best form (R11),
neither container will hold them. Confidence: low to medium.

**R15. The prompt distilled into the weights by self-distillation.** The same base model with the full prompt is
the teacher; the adapter with a short prompt is trained to match its output distribution on recorded contexts (a
divergence loss needs no labels, no external teacher and no terms-of-service question). The gain is context room
and a prompt that stops growing, **not time**: the prompt's state is cached on disk. Low priority for that reason.
Measure: decision agreement between short-prompt-plus-adapter and full-prompt base on held-out contexts; codebench
as in plan item D4. Confidence: medium that it works, low that it matters.

**Rejected:** reinforcement learning with test rewards (compute [33]); forward-only optimisation (thousands of
steps [74]); a hypernetwork that writes adapters (must be trained per base; none exists for these models [45]);
re-quantizing a merged model as the default product before R6 shows it keeps the update; a judge model for
filtering data (a second model in memory; soft verification is enough [32]).
