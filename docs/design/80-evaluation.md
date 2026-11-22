# Evaluation

How to know, on one slow machine that drifts, whether a change made the agent better. Every other document ends its
proposals with "measure"; this one defines the instruments.

## Where katana is today

- `tools/codebench/`: 51 tasks with hidden tests (15 small fixes, 6 multi-file, 5 terminal, 4 web, 9 news, 12
  code-writing added on 2026-11-21 and never run with a model). `katana codebench verify` proves each task fails untouched
  and passes with its reference solution. A run of the first 21 takes 40 to 50 minutes per model (109 to 139 s per
  task, measured). Limit per task: 900 s of wall time.
- Results so far: four configurations on 21 tasks, one run each, 18 or 19 solved in every one; the project's own
  reading is that nothing is significant. The terminal and web tasks are solved by both models and separate
  nothing.
- `katana agentlog` reads the agent log, which the tool tests polluted (`01-token-budget.md`).
- For training: training loss only. No held-out loss was ever computed on a real model.

## What the frontier does

- **Deployed.** The best-known public benchmark was abandoned by a major vendor in 2026: of the problems its
  strongest model did not solve reliably, 59% had flawed tests or descriptions, and contamination was shown for
  every frontier model; its intended successor was audited and about 30% of the tasks were found broken [84]. An
  audit of 254 submissions found that exact paired tests separate none of the adjacent top-30 pairs [77].
  Infrastructure alone (resource limits, time of day) moved a terminal benchmark by 6 points [85].
- **Published.** Report standard errors, compare paired, cluster by task, do a power analysis [86]; normal-theory
  intervals are wrong under a few hundred items [86]. For regression-testing one agent across versions, a 10%
  subset chosen from a previous full run keeps the error under 5% at a tenth of the cost [87]. Same-task runs vary
  by up to 30 times in tokens; tokens per resolved task varies 20 times between harnesses for one model [76][81].
- **Unsolved.** A standard metric for call validity. Evaluation without tests that can themselves be wrong.

**For a slow engine:** a full benchmark run is a night, a difference of one task is noise, and public benchmarks
are neither affordable nor trustworthy. The design is a ladder of instruments from minutes to a week, each gating
the next, built on the owner's own data, with behaviour metrics that have hundreds of observations per run instead
of 21.

## Design

### Now

**E1. Offline decision replay: `katana replay`.**
- What: a held-out set of recorded sessions (the teacher's turns in the owner's sessions and imported transcripts;
  fixed, never trained on). The engine feeds each session once, teacher-forced, and at every model turn records,
  without generating: (a) whether the model's most likely tool name equals the reference's; (b) the mean loss on the
  decision tokens (R2 of `60-training.md`); (c) whether the most likely first token after the reasoning block opens
  a call or text, against the reference. Output: three numbers with a bootstrap interval over sessions, and a
  per-turn file for paired comparison between two configurations.
- Mechanism: feeding runs at prompt speed and every position of a session is an observation: a 30,000-token
  session yields about 150 decisions in about 8 minutes (estimate from the measured prompt speed). The same
  positions are scored before and after a change, so the comparison is paired and the drift of the machine does
  not matter (no timing involved).
- Limits: agreement with a reference is not task success. A change can raise agreement and solve nothing. Replay
  gates (a training night that does not move it is not worth a benchmark run); it does not decide.
- Cost: `src/app/replay.c`, about 250 lines, on top of the importer's span logic and `agent_feed`. Three days.
- Validate the instrument itself: the base model with and without tool declarations must differ clearly on (a);
  if it does not, the instrument is blind and is fixed before it is used.

**E2. The micro-set.**
- What: 18 tasks: the 12 code-writing tasks plus the 6 of the first 21 that have failed at least once (`calc`,
  `ledger`, `mdhtml`, `lru`, `todo`, `kvstore`). Revised once after the first runs: tasks every arm solves in
  every run leave the set; tasks whose hidden test rejects a correct solution are fixed (read every failure of the
  first run by hand; the public benchmarks' main defect was tests that reject valid work [84]).
- Protocol: arms alternate task by task (A on task 1, B on task 1, B on task 2, A on task 2, ...), two passes, same
  session of the machine, nothing else started during the run. Limits per task in **generated tokens and rounds**
  (12,000 tokens, 60 rounds), not wall seconds: the recurring `calc` "timeout" is the machine's load, not the model
  [85].
- Recorded per task and arm, from `katana tokens`: solved, rounds, tokens written and read, failed results by
  kind, grammar corrections, model seconds.

**E3. How to read a comparison.**

| metric | unit of analysis | test | what two passes of 18 tasks can detect |
|---|---|---|---|
| solved | task (majority of passes, ties broken by a third pass) | exact sign test on discordant tasks | only large effects: 6 discordant tasks all one way gives p = 0.03 |
| failed-result rate, corrections per 100 calls | call, clustered by task | bootstrap over tasks | about 10 points (estimate: 300 to 500 calls per arm, design effect 2 to 3) |
| rounds, tokens, model seconds per solved task | task | paired bootstrap over tasks, on the log ratio | about 20% (estimate; same-task token use is heavy-tailed [81]) |

Rules: one primary metric named before the run; an interval, not a point; "no difference detected" is written as
that, with the size that could have been detected; a result that decides a default is repeated on another day.

**E4. The ladder.**

| step | instrument | cost | gates |
|---|---|---|---|
| 1 | model-free tests (`tools/tooltest`, `traintest`) | seconds | merging code |
| 2 | replay (E1) | 10 to 30 min | spending a micro-set run; accepting a training night |
| 3 | micro-set, two passes, two arms (E2) | 3 to 5 h (estimate) | changing a default |
| 4 | all 51 tasks, two passes | a night per arm pair | releasing a specialization or a base-model change |
| 5 | a week of the owner's use, `katana tokens` over the sessions | free | closing a plan item ("in real sessions ...") |

**E5. Weekly numbers from real use.** `katana tokens ~/.katana/desktop/sessoes --since 7d`: requests, share
closed with text, rounds per request, failed-result rate by kind, repeat rate, re-read rate, calls per turn,
tokens per request, share of requests that hit a round limit. This replaces `katana agentlog` for plan items A2,
A3, A4 and A8. Observational: it shows trends and regressions, not causes. The tool tests must write to a
temporary log (plan item A5 is not closed for `tools/testar_ferramentas.sh`).

### Next

**E6. Tasks from the owner's own repositories.** Held-out mutation tasks (R13 of `60-training.md`) from projects
with tests: uncontaminated by construction, in the owner's languages and style, and refreshed by generating new
mutations. A fixed held-out set of 30, never used for collection. Measure of the instrument: both reference models
score between 20% and 80% on it (otherwise it separates nothing).

**E7. A regression subset chosen from data.** After five full runs, pick the tasks whose outcome varied between
configurations and drop those that never changed, as in [87]. The micro-set of E2 is the hand-made first version.

### Beyond

**E8. Counterfactual replay with a recorded world.** Re-run a recorded request with the tools answered from the
recording while the model's calls match it, and stop at the first divergence: "how many rounds of a successful
trajectory does this configuration reproduce". Costs generation, but no tool time and no task setup, and applies
to any real session. Confidence: low; divergence is not failure.

**E9. A simulated environment.** One of the model files on disk is, by its authors' description, a model of
environments: it predicts tool output from an action and a history [26]. As a simulator it could produce varied
tool results for the curriculum generator or for stress tests of the loop guards, without touching the machine. It
needs the engine for itself while it runs. Confidence: low; listed because the file is already there and is being
used for something else (`99-review-of-current-work.md`, item 1).

**Rejected:** a judge model (a second model in memory, and a judge of this size is not better than a test);
public benchmark runs as a goal (days of machine time per run, contaminated and flawed tasks [84], and scores that
depend on the harness as much as the model [75][76]); wall-clock limits as pass criteria.
