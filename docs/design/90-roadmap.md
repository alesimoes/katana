# Roadmap

One ordered list across the subsystems. The order is expected gain in seconds per finished piece of work, divided
by the work to build it, under the slow-engine constraint, with the measurements that decide forks placed first.
Item codes refer to the subsystem documents (L: `10-agent-loop.md`, T/V/B: `20-tools.md`, F: `30-call-formats.md`,
C: `40-context-and-memory.md`, X: `50-instructions-skills-mcp.md`, R: `60-training.md`, I: `70-inference.md`,
E: `80-evaluation.md`). Gains are estimates from `01-token-budget.md` unless marked measured.

## 0. Before anything else: three experiments

They need no new code (only the accounting script in `docs/design/appendix/`), and their outcomes decide the
largest forks: which base model everything is trained and tuned on; whether a night of training moves behaviour
at all and whether facts go in; and how much the context policy is costing. Run them with the machine otherwise
idle (no training run, no other agents), which is itself a decision for the owner: experiment 1 costs about one
training night.

### Experiment 1: the base model and reasoning

- **Question.** Which model should be the agent and the base of every specialization, and should it reason?
- **Why first.** The everyday model is a world model by its authors' description (`99-review-of-current-work.md`,
  item 1). Every training night and every loop mechanism is tuned to its base.
- **Arms.** A: Qwen3.6, `KATANA_THINK=0`. B: Qwen-AgentWorld, `KATANA_THINK=0` (the desktop's configuration on
  2026-11-21). C: Qwen3.6, `KATANA_THINK=1`. D: Gemma 4, default. No specialization in any arm.
- **Tasks.** The micro-set of `80-evaluation.md` (E2): `write-*`, `prereq-*`, `extend-*` and `calc`, `ledger`,
  `mdhtml`, `lru`, `todo`, `kvstore`: 18 tasks.
- **Commands** (one task at a time so that arms alternate inside the same quarter hour; repeat the whole loop for
  pass 2 with the arm order reversed):

  ```
  Q=models/Qwen3.6-35B-A3B-UD-Q4_K_M.gguf; W=models/Qwen-AgentWorld-35B-A3B-UD-Q4_K_M.gguf
  G=models/gemma-4-26B-A4B-it-UD-Q4_K_M.gguf; R=tools/codebench/run.py
  for t in <the 18 task names>; do
    python3 $R --model $Q --tasks $t --label A --env KATANA_THINK=0 --out e1/A.jsonl --sessions e1/A --timeout 1800
    python3 $R --model $W --tasks $t --label B --env KATANA_THINK=0 --out e1/B.jsonl --sessions e1/B --timeout 1800
    python3 $R --model $Q --tasks $t --label C --env KATANA_THINK=1 --out e1/C.jsonl --sessions e1/C --timeout 1800
    python3 $R --model $G --tasks $t --label D --out e1/D.jsonl --sessions e1/D --timeout 1800
  done
  python3 docs/design/appendix/session_tokens.py e1/A --tsv > e1/A.tsv     # and B, C, D
  ```

  Check first that `run.py` appends to `--out` across invocations; if it overwrites, give each call its own file.
  The 1800 s limit is deliberately loose: wall time must not decide a task on a drifting machine.
- **Record** per task and arm: solved, rounds, failed results, repeated calls, text turns, tokens written and
  read, wall seconds, load average at start.
- **Size.** 18 tasks x 4 arms x 2 passes = 144 runs. At the 2 to 5 minutes per task measured on the older tasks
  this is 5 to 12 hours (estimate; the new tasks have never been run). If the first pass shows an arm far behind
  (fewer than half the tasks of the best arm), drop it from pass 2.
- **Reading** (`80-evaluation.md`, E3). Primary: failed-result rate and rounds per solved task, bootstrap over
  tasks. Secondary: solved (sign test on discordant tasks), share of runs ending with text.
- **Decisions.**
  - B behind A on failed-result rate by 10 points or more, or on solved tasks with 6 or more discordant tasks one
    way: the desktop default becomes Qwen3.6, `tools.kspec` is archived, and all training targets Qwen3.6.
  - B not behind: keep it, and experiment 2a evaluates the specialization already trained on it.
  - C against A decides the default of L4 (`10-agent-loop.md`): if C solves more or fails fewer calls at a cost
    of less than 30% more model seconds per solved task, reasoning is on at the first turn and after failures;
    if C is equal and dearer, off with escalation only.
  - D against the best Qwen arm decides which family gets the first specialization.
  - Tasks solved by every arm in both passes leave the micro-set; tasks solved by none are read by hand for a
    hidden test that rejects a valid solution.

### Experiment 2: does a night of training move anything, and do facts go in

**2a. The night already spent.** `~/.katana/training/tools.kspec` (75 steps, 7.9 hours, base AgentWorld, trained
with the short prompt) has never been evaluated.

- **Arms** on the experiment-1 micro-set, AgentWorld as base (whatever experiment 1 says; this specialization fits
  no other base): B0 base with declarations (reuse arm B if run on the same day, otherwise rerun); S1
  `KATANA_SPEC=tools.kspec` with declarations forced on (`KATANA_TOOL_DECL=1`); S0 the same with the declarations
  off, as it was trained.
- **Also record**, alternating three times each: `katana bench <model> 256 128` with and without `KATANA_SPEC`
  (the serving cost of a specialization, R6 of `60-training.md`).
- **Size.** 18 tasks x 2 new arms x 2 passes: 3 to 6 hours.
- **Decisions.** S0 at least as many tasks as B0 and fewer `error` results: the no-declarations path is alive;
  repeat on the chosen base. S0 worse and S1 equal to B0: training at this size changed nothing measurable;
  no further mixed nights; R1 to R5 (`60-training.md`) are built and the GPU measurement (R7) is done before the
  next night. S1 worse than B0: the recipe damages the model; stop and find out why with the replay probe.
  Generation slower with the specialization by more than 5%: unmerged adapters before any use.

**2b. Facts.** One project of the knowledge base with 30 to 60 episodes.

- **Data.** `katana knowledge corpus ~/.katana/knowledge kb.jsonl --project <P> --max-per-project 80 --holdout
  kb-held.jsonl`; a control file built the same way from a second project that is **not** trained
  (`--project <P2> --holdout kb-ctl.jsonl`, training file discarded).
- **Before.** Ask every held-out and control question to the chosen base, reasoning off, temperature 0, 120
  tokens each (`katana chat <model> @question.txt 120 0`, one process per question, or the server with a script).
  Score without a judge: an answer is *correct* when it contains every key string of the expected answer (the
  paths and the first word of each command; extracted mechanically), *specific-wrong* when it names a path or
  command that is not in the expected answer, *abstained* otherwise.
- **Train.** `katana specialize <base> knowledge --corpus kb.jsonl --steps 100` for two nights (the second
  resumes). Keep a 20-window validation file out of `kb.jsonl` and compute its loss before and after.
- **After.** The same questions, the same scoring, with `KATANA_SPEC`.
- **Size.** 80 held-out plus 80 control questions x 2 (before, after) at about 10 s each: under an hour. Two
  nights of training.
- **Decisions.** Held-out correct rises by 30 points or more and control specific-wrong does not rise by more
  than 5: facts as questions and answers work at this budget; continue the corpus project by project and build
  R14 (per-project adapters). Held-out rises by less than 10 points: question-answer facts are dropped; build R11
  (operational facts) and run this protocol again with first-call agreement as the score; if that fails too, the
  owner is told that project knowledge does not fit in the weights at this budget and the choice is GPU training
  or the brief. Control specific-wrong rises by more than 10 points: training on facts makes the model invent;
  stop regardless of recall.

### Experiment 3: what the context policy costs

- **Question.** How much of the repeated work comes from compacting at 6,000 tokens, and how much speed does a
  deeper context cost?
- **Part 1, speed at depth** (engine only). `KATANA_BACKEND=vulkan ./katana bench <model> <P> 64` with P = 2048,
  4096, 8192, 16384, 32768 (with `KATANA_CTX` set above P), three alternating rounds, both models. Record prompt
  and generation tokens per second. About an hour.
- **Part 2, behaviour.** Arms on the six longest tasks of the micro-set (the three `extend-*` and the three
  `write-*` with the most rounds in experiment 1), chosen base and reasoning setting from experiment 1:
  K default; N `KATANA_COMPACT=0` with `KATANA_CTX=32768`; D `KATANA_DIGEST=0` (reading notes off, compaction
  default). Three passes (six tasks give few observations per pass).
- **Record** as in experiment 1, plus from the sessions: reads of a file already read and unchanged, repeated
  calls, and from stderr the count and duration of compactions (the lines printed by `agent_compact`).
- **Size.** 6 tasks x 3 arms x 3 passes = 54 runs: 3 to 6 hours.
- **Decisions.** N has fewer repeated reads and calls and no more model seconds per solved task than K: the
  trigger moves to the depth where part 1 shows a 15% loss of generation speed, and C2/C3
  (`40-context-and-memory.md`) are built before any other context work. N is slower overall because of depth:
  the amnesia is real but must be solved without depth, so C4 (scopes) moves ahead of C2/C3. No difference: the
  repeats are the model's, not the window's; context work drops below the tool and training items. D against K
  decides whether reading notes stay on (drop them unless D is worse).

## 1. The ordered list

Dependencies in brackets. "Gate" is the measurement that must pass before the item becomes a default.

| # | item | gain (estimate unless marked) | work | gate |
|---|---|---|---|---|
| 1 | Experiments 1, 2a, 3 | decide the forks | machine time | n/a |
| 2 | `katana tokens` and times in the session file (`01-token-budget.md` section 4) | the instrument | 3 days | matches the appendix script |
| 3 | L7: every note switchable and counted | makes item 1's follow-ups readable | half a day | n/a |
| 4 | T1: short result wrapper | 8,250 tokens read per reference session (measured size) | 1 day | failed-result rate not higher |
| 5 | L2: closing turn with calls forbidden | every request ends with text | 1 day [call grammar merged] | E5 share closed with text |
| 6 | F1: call grammar on, measured | the 14 argument failures of the session, and the base for 7, 13, 17 | measurement | rejected calls per 100 fall; solved not lower |
| 7 | L1: fault kinds | failed-after-failed rate; base for R10 | 2 days | next-call success +10 points |
| 8 | E1: replay probe | makes training nights measurable in minutes | 3 days | separates declarations on/off |
| 9 | R3, R4, R7, R6: validation, real resume, GPU step time, serving cost | stops blind nights; possibly a 3x budget | 2 days + measurements | n/a |
| 10 | T2 + T3: result shaping with handles | up to half of `Bash` output: about 14,000 tokens read per session | 1.5 weeks | seconds per solved task fall; handle opens under 40% |
| 11 | V1: verdict-first edits [brief `test:`] | one round and one log per edit | 1 week | rounds per task -10% on `extend-*` |
| 12 | C2 + C3: ledger and masking instead of the summary [10] | no generated summaries; fewer re-reads | 1.5 weeks | experiment 3 arm added: solved not lower, re-reads fall |
| 13 | F2 + F3: canonical order and forced tokens [6] | about 0.4 s per call | 4 days | 0.15 s per call measured |
| 14 | R1 + R2 + R5: one target per night, decision spans, shared-expert adapters [8, 9] | 3 to 5 times more decisions per night | 1 week | probe moves in two nights |
| 15 | T4, T5, L6, X1, X2, X3: unchanged-read stub, engine descriptions, plan deltas, lean brief | a few thousand tokens per session together | 1 week | each by its own counter |
| 16 | L3 + L4: plan gate at first change; reasoning by escalation [experiment 1] | fewer wasted rounds on weak plans and after errors | 4 days | micro-set, three arms each |
| 17 | F5: state-grounded constraints [6] | not-found and mismatch failures impossible (8 of 81 failures; 3 of 10 edits) | 2 weeks | wrong-but-valid edits fewer than mismatches removed |
| 18 | I1 + I4: time split of a token and of a batch; threads | directs all engine work; prompt speed is 45% of model time | 2 days | n/a |
| 19 | I2: copy drafts in the agent [17 helps] | edits and rewrites at reading speed | 1 week or more | -20% seconds per edit |
| 20 | R9 + R10: grammar as teacher, failure-driven curriculum [6, 7, 14] | the curriculum is the day's own mistakes | 2 weeks | corrections and failures per 100 fall by a third |
| 21 | C4: snapshots and scopes | compaction with no generation and no recomputation; base for L9, L12, I2's restore | 2 to 3 weeks | restore cheaper than re-feeding 500 tokens; solved not lower |
| 22 | B1 + F6: batch programs [6] | rounds on dependent calls | 2 weeks | used in over 5% of rounds; no more failures than single calls |
| 23 | X4, X5, X6, X7: engine-run steps, self-loading skills, cards for own tools, shaped server results | context room; rounds per skill | 1.5 weeks | each by its counter |
| 24 | R11, then experiment 2b again; R14 or C6 by its result | project knowledge in a form the budget can move | 1 week + nights | first-call agreement on held-out requests |
| 25 | L8, L9, L10 [11, 21] | done means verified; scoped exploration; restart from the ledger | 1 week | by item |
| 26 | T7: anchored edits | half the tokens of an edit | 4 days | +10 points success or -30% tokens on the edit benchmark |
| 27 | plan C1, C3 as directed by 18; I6 in the simulator | generation and prompt speed | as planned | alternating benches |
| 28 | L11, L12, X9, X10, R12, R13, E6, I8 | bets and instruments | by item | by item |

What moves the order: experiment 1 choosing a new base moves 14 and 20 after a baseline night on that base;
experiment 2a showing that training moves nothing puts 9 (GPU step time) directly after 2; experiment 3 showing no
effect of the window moves 12 and 21 below 22.

## 2. The original designs, collected

As far as the landscape report shows, no shipped system has these. Confidence is the architect's, not a
measurement.

| # | design | where | confidence |
|---|---|---|---|
| 1 | Scopes by state rollback: a finished plan step or exploration is cut from the context by restoring a snapshot and feeding one ledger line; no summary, no recomputation | C4 | medium to high (mechanism), medium (behaviour) |
| 2 | Verdict-first edits: the engine runs syntax, lint and tests inside the edit and the model reads one verdict line | V1 | high |
| 3 | State-grounded constrained decoding: paths that exist, `old_string` that occurs in the file, live handles, enforced as sampling masks | F5 | medium to high |
| 4 | The grammar as a teacher: every decoding correction is a certain label for the night's training | R9 | medium to high |
| 5 | Failure-driven nightly curriculum with hindsight: the day's failed calls, rewritten as the call that later succeeded | R10 | medium |
| 6 | The engine ledger as the compaction format, model-free, taught by the curriculum | C2 | high |
| 7 | Batch programs in a closed mini-language, valid by construction under the call grammar | B1, F6 | medium |
| 8 | Round fusion from the owner's own call statistics: the engine appends the result of the call that would come next | L11 | medium |
| 9 | Loss on decisions, with training text shaped as at run time | R2 | medium to high |
| 10 | Operational facts: project knowledge trained as the first call it implies, keyed on the environment block | R11 | medium |
| 11 | A trained state per project (recurrent state plus key-value prefix) instead of weights for facts | C6 | low to medium |
| 12 | Speculative next turn under the passing verdict while the tests run | L12 | low to medium |
| 13 | Rule accounting: per-line violation counts with and without the line in the prompt; lines that do nothing leave | X9 | medium |
| 14 | Skills and rules that graduate into the weights when the probe says the text is no longer needed | X11 | low to medium |
| 15 | Sampler-level turn control: rules moved from refusal after the call to masks before it | F7 | medium |
| 16 | Token-local expert prediction keyed by grammar phase | I6 | low |

## 3. Ideas examined and rejected

| idea | reason |
|---|---|
| Precomputed KV states for common tool-result prefixes | the state at a position depends on all earlier tokens (attention and recurrent layers); only whole prefixes are reusable, which is what the prefix cache and scopes do |
| A learned "plan token" vocabulary; one-token calls | new embeddings need full backward passes at 245 s per window and break the native formats; forced tokens (F3) and plan deltas (L6) get most of the saving for nothing |
| Session-level router statistics to preload experts | measured: a heat file from another session is slightly worse than none, a fixed calibrated set is near chance, the best fixed set equals the current policy (`docs/handoff.md` 0j); only token-local prediction has headroom (I6) |
| Removing the system prompt's tools section as a time saving | the prompt's state is cached on disk (0.05 s); the gain is context room only, the risk is call accuracy; cards (X6) first |
| Facts as question-answer pairs at 20% of a mixed night | no published support at this scale; wrong form for an agent; decided by experiment 2b |
| Reinforcement learning with test rewards; forward-only training; adapter-writing hypernetworks | compute far beyond the machine, or no model for these bases [33][45][74] |
| A semantic code index; a fast-apply edit model; a judge model; a draft model | each is a second model in memory or a second architecture |
| Parallel sub-agents and agent teams | one engine: no wall-clock gain, several times the tokens [88] |
| A planner/executor split | no gain for small models in practice [79]; one model in memory |
| Model-written reading notes by default | generation costs 2.6 times reading; measured once at 63 s against 45 s |
| Compact table encodings for model-written text; "telegraphic" answer styles as a prompt rule | independent studies: accuracy losses in multi-turn tool use, savings far below claims [54][55] |
| Text rendered as images to cut tokens | no better than truncation at equal budgets in independent studies [63] |
| Prompt compression by a second model | evidence on tool-using agents is negative or missing [53] |
| Converting Qwen3.6 calls to JSON | code inside JSON strings lowers code quality [7] |
| Public benchmark runs as a goal | flawed and contaminated tasks; scores depend on the harness; days of machine time [84][75] |
| More slot-policy tuning; dense weights on the CPU to open slots | measured flat or worse |
| Latent or continuous "thought" tokens | every method that works needs training the base; the training-free ones need embedding mixing the engine does not have, with disputed gains [52] |

## 4. What this roadmap does not cover

The desktop application, packaging, the Windows build, the network API's client side, and plan blocks E and F.
They are product work whose order the owner sets; nothing here depends on them except that API sessions must obey
the brief's rules (`99-review-of-current-work.md`, item 10).
