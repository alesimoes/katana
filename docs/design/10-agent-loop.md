# Agent loop

How a request becomes rounds, when the loop plans, thinks, stops and recovers. Token and round numbers are from
`01-token-budget.md`.

## Where katana is today

One loop, `agent()` in `src/agent/loop.c`: feed the pending text, generate (`gen_console`, `src/app/console.c`),
parse every call of the turn, run each through `agent_tool`, append results, repeat. Around it, added on
2026-11-21 and **none measured with a real model**:

| mechanism | where | what it does |
|---|---|---|
| round limit | `rl_block`, `lim` | blocks of 40 rounds, another block if a file was written or a step completed, cap 200 |
| repeat handling | `rp_sig`, `rp_res`, `rp_err` | note on an identical call with an identical result; refusal of a call that failed twice; forced end after two refusals; note after four failures in a row |
| blocked prerequisite | `bk_key_of`, `bk_count` | 3rd and 6th failure around one program or folder get a note |
| plan first | `pl_calls`, `plan_cur` | notes at the 3rd and 6th call without a task list; current step every 5th result |
| open steps reminder | `nudged` | one reminder when the model stops with unfinished steps |
| closing | penultimate-round note | tells the model to stop calling tools and report |
| sub-agent | `tool_delegate`, `src/agent/delegate.c` | saves the KV state to disk, runs a read-only sub-task from position 0, reloads |
| plan mode | `src/agent/planmode.c` | read-only until `/go` |

All of these act by appending English prose to a tool result. In the reference session the model made 0.99 calls
per turn, 39% of results failed, four of six requests ran into the round limit, and two requests produced no text.

## What the frontier does

- **Deployed.** The strongest agents are a single loop with planning as a tool, not a planner component; a
  practitioner study removed a planner, a state machine and model routing from a small-model harness at no loss
  [78][79]. The harness matters about as much as the model: the same small model differed by up to 44 points of
  resolved tasks and 20 times in tokens per resolved task between harnesses [75][76]. Long jobs are held together by
  durable artifacts the harness owns (a progress file, a feature list whose only editable field is a pass flag, a
  smoke test at the start of each session) [65]. Sub-agents buy context isolation; parallel teams multiply tokens
  (vendor figures of 4 to 15 times) [88].
- **Published.** Planning is an accuracy aid for weaker models and a cost saver for strong ones [78]. Starting at
  the lowest reasoning effort and escalating only after a verified failure matches fixed high effort at a fraction of
  the cost; on tasks every level solves, top effort costs up to 42 times the lowest [80]. Delivering a failed call
  with its exact fault kind raised tool success by 10 to 14 points on open models of 7 to 70 billion parameters
  [83]. A loop is an identical call with an identical observation, not a repeated call [89]. The Qwen3.6 authors
  state that the model was trained to reuse reasoning kept from earlier turns and recommend keeping it in agent use
  [25].
- **Unsolved.** No controlled measurement of what loop guards buy. No published policy for when a small model should
  reason. Harness effects are confounded with model effects in almost all evidence.

**For a slow engine:** every mechanism above that costs a round or a paragraph must earn it, and the loop has two
resources hosted loops lack: the sampler (it can forbid a call instead of asking the model not to make one) and the
state (it can roll back).

## Design

### Now

**L1. Fault kinds on every error.**
- What: every failing result starts with `error[kind]:` where kind is one of `args`, `unknown-tool`, `not-found`,
  `usage`, `exit`, `denied`, `timeout`, `mismatch`, `refused`, `cut`. One line of what to change follows.
- Mechanism: the model conditions on a small closed vocabulary instead of parsing free text; the engine keys repeat
  and prerequisite logic on the kind instead of on substrings (`bk_blocked_text` greps for 12 English phrases
  today); training (`60-training.md`, T3) can target each kind.
- Evidence: [83]. Cost: `ct_fail(kind, fmt, ...)` in `src/tools/common.c`, replace the `"error: "` literals (about
  200 sites, mechanical), `rp_failed` and `bk_*` in `loop.c` read the kind. Two days.
- Measure: failed-result rate and failures-after-a-failure (does the next call succeed?) on the micro-set, kinds on
  and off. Drop if the after-failure success rate does not move by 10 points.

**L2. A closing turn the model cannot escape.**
- What: when a request must end (round limit, second refusal, or the model ended a turn with neither text nor a
  call), the engine generates one more turn with the call opener forbidden in the sampler.
- Mechanism: P6. Today the engine asks, in a note, for a text answer; in the reference session 115 of 215 turns had
  no text. Forbidding the opener token (`<tool_call>`, `<|tool_call>`) leaves text as the only continuation.
- Cost: one flag checked where `cg_check` is called in `gen_console`; set by `agent()` at `round == lim - 1` and in
  the `!calls` branch when the reply is empty. One day.
- Measure: share of requests that end with text (plan item A2), from `katana tokens --by request`. Drop: never if it
  reaches 100%; it is the cheapest fix for the owner's most visible complaint.

**L3. The plan gate moves from "third call" to "first change".**
- What: no plan note while the model only reads. When the first mutating call (`Write`, `Edit`, a `Bash` command
  that is not on the read-only list of `planmode.c`) arrives in a request that has made 3 or more calls and has no
  task list, the engine does not run it and answers `error[plan]: write the plan first (TodoWrite, one line per
  step), then repeat this call`. A request answered within 2 calls never sees it.
- Mechanism: a plan written after exploring is a plan; one demanded at call 3 is a guess. Enforcement instead of a
  note (P4). It also stops charging plan overhead to questions ("is everything installed?" took 24 rounds and would
  have been told to plan).
- Evidence: planning helps weaker models [78]; no evidence on gate position. Cost: 30 lines in `agent()`, reusing
  `plan_cmd_ok` (`src/agent/planmode.c`). Measure: on `write-*` and `extend-*` tasks, gate at first change against notes at calls 3
  and 6 against none; solved tasks, rounds, share of requests with a plan. Drop the gate if it costs more than one
  round per request on average with no gain in solved tasks.

**L4. Reasoning by escalation, with a hard cap.**
- What: three states per turn: off (the empty block is fed as prompt, as today), short (cap of 120 reasoning
  tokens), long (cap of 600). Default off. Short on the first turn of a request and on the plan turn. Long for one
  turn after two consecutive failures or a refused call. At the cap the sampler forces the closing tag. Reasoning of
  earlier turns is kept in the history, as the model's authors recommend [25].
- Mechanism: reasoning is the only tool the model has to change course after an error, and it costs 40 ms a token;
  spend it where the evidence says it pays [80].
- Cost: `think_pre` (`src/app/console.c`) takes the state; the cap is a counter in `gen_console`; `ah_render` keeps
  earlier reasoning (it does already; compaction strips it in `ah_summary`). Two days.
- Measure: plan item B1 as a three-arm comparison (off, escalation, always on) on the micro-set: solved, seconds
  per solved task. Drop escalation if always-off solves as many.

**L5. Several calls per turn as the normal case.**
- What: the loop already runs every call of a turn. Make it the default behaviour: one prompt line, curriculum
  family (`60-training.md`), and the plan-step note names independent reads together.
- Mechanism: P3. The teacher makes 2.3 calls per turn on the same tools; the local model 0.99.
- Measure: calls per turn and rounds per solved task. Drop: nothing to drop; if training does not move it, B1 of
  `20-tools.md` (batch programs) is the enforcement path.

**L6. Plan progress as deltas.**
- What: `TodoWrite` accepts `done: 2` and `now: 3` (one line) besides the full list; steps that carry a check
  (`=> command`, as in the skill format) are marked done by the engine when that command exits 0.
- Mechanism: the teacher session spent 8,500 tokens rewriting the list 46 times (186 tokens per call). A delta is
  about 8.
- Cost: `src/tools/tasks.c`, 40 lines; the call grammar gets the alternative. Measure: tokens in `TodoWrite` calls
  per request. Drop if models keep sending the whole list after the curriculum.

**L7. Every note switchable and counted.**
- What: `KATANA_NOTES=<list>` enables notes by name (`plan`, `repeat`, `blocked`, `streak`, `step`, `close`); each
  note fired is recorded in the session (`KSX2`, `01-token-budget.md` section 4).
- Mechanism: P5. Six mechanisms were added in one day and cannot be told apart in a result.
- Cost: half a day. Drop: never.

### Next

**L8. Done means verified.** The `done:` and `test:` commands of the project file are run by the engine when a
request that changed files is about to close; the verdict (see `20-tools.md`, V1) is appended to the closing turn's
input, and a failing verdict reopens the request once. Replaces the unwired `brief_end_note`. Measure: share of
closed requests whose tests pass on `extend-*`. Drop if the reopened round fixes nothing in 8 of 10 cases.

**L9. Exploration as a scope, not a sub-agent.** `Agent` (read-only sub-task) is reimplemented on state rollback
(`40-context-and-memory.md`, C4): the sub-task runs in the parent's context with no prompt re-read, and when it
reports, the engine restores the state from before the scope and appends only the report. Measure against today's
`tool_delegate` (measured once: 152 s for a job that kept the parent 5,500 tokens smaller): seconds and parent
context growth. Drop if the snapshot costs more than the 2,400-token prompt reload it avoids.

**L10. Long work survives the session.** The engine ledger (`40-context-and-memory.md`, C2) is written to
`.katana/progress` in the project; a new session on the same folder starts from it. No model call to write it.

### Beyond

**L11. Round fusion from the owner's own call statistics (original).**
- What: from the session files, the engine computes P(next call | this call, result class). When a successor is
  read-only, cheap, and follows with probability above a threshold (candidates in the data: `Edit` then `Read` of the
  same file; a file-list search then `Read` of the first hit; `Write` then a syntax or test run), the engine runs it
  and appends its shaped result to the first result, marked `[also: Read x.c 40-80]`.
- Mechanism: P3 without asking anything of the model: the round the model would have spent is removed when the
  prediction is right and costs read tokens when it is wrong.
- Evidence: none for this mechanism. Compound tools designed by hand cut call counts in small-model harnesses [67];
  a hosted vendor measured that shortening can raise total cost [10], which is the risk here in reverse (longer
  results).
- Cost: a table built by `katana tokens --bigrams`, a hook after `agent_tool` in `loop.c`, a byte budget per fused
  result. One week.
- Measure: rounds and total tokens per solved task with fusion on and off; precision of the prediction (was the
  fused result used: did the model skip the call?). Drop if tokens per solved task rise, or precision is under 60%.
- Confidence: medium.

**L12. Speculative next turn while a verification runs (original).**
- What: when the engine starts a verification whose passing verdict is a fixed string (V1 in `20-tools.md`), it
  snapshots the state, feeds the passing verdict and generates the next turn while the tests run. If they pass, the
  turn is already written; if not, roll back and feed the real verdict.
- Mechanism: P6. Tool time and model time overlap instead of adding; only possible because the result text is known
  in advance and the state can be restored.
- Cost: needs C4 (snapshots) and V1; then 100 lines in `agent()`. Measure: wall seconds per request on `extend-*`
  where tests take over 5 s. Drop if tests pass less than half the time after an edit (the speculation would
  mostly be thrown away while competing with the test for CPU).
- Confidence: low to medium; the gain is bounded by test duration.

**Rejected here:** a separate planner model or planner/executor split (one model in memory; no evidence of gain for
small models [79]); parallel sub-agents (one engine); a judge turn after every request (a full extra generation; the
test verdict is the judge).
