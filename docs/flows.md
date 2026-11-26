# Flows: declared playbooks, the per-turn `next` line, and state-guided retrieval

Written 2026-11-25. **Nothing in this document was measured, and none of it ran with Qwen3.6.** It compiles; the
state machines were stepped only by the scripted simulator (`katana flow sim`, no model). Every number is a design
value. Research background: `docs/research/beyond-rag.md` (its sources marked [V]/[L] were not verified in full).

Code: `src/agent/flow.c` (registry, state, the line), `src/agent/flowmem.c` (miner, lessons, retrieval, probe),
hooks in `src/agent/loop.c`, notes `flow_next`, `flow_evidence`, `flow_decl` in `src/agent/knote.c`.

## 1. Why: the failures of 2026-11-25

Lines are from `~/.katana/agente.log`. One day, 326 calls, 72 failed.

| # | Failure | Evidence (line) | Cause in the engine | Status |
|---|---|---|---|---|
| 1 | The same task list resent 30 times, each refused with "the list did not change" | 10054-10080, 10124-10131, 10273-10282 | the model marked steps with a trailing ` - DONE`; the parser only knew `[x]`, so the list was "equal". The error named `done=N` but nothing changed across requests | fixed: suffix accepted; `todos` + `done` in one call honoured |
| 2 | Identical command, identical result, again and again, across user messages | 10141-10145 (`find *.xip` x5), 10112-10156 (same login x8), 10266-10281 (`--version` triple x4), 10316-10318 | repetition control (`rr_`, `rp_`) is reset at every user message; Bash was never covered | built: conversation ring + stall by evidence gain (sections 3.3, 3.4) |
| 3 | Same search + same empty page for hours | 10171-10190 (`xtool.sh anisette server` x8, a 13-byte page x6) | a 13-byte page counted as success; no memory of dead ends across requests | page quality was fixed during the day (`[web: ...]`); built: web flow with a call cap and dead-end count |
| 4 | Web search for the local program, then a made-up address | 10312-10321 (`katana render brief`, `katana.readme.io`) | nothing told the model where it was in the image job, so "unknown command" became "search the web" | built: image flow; WebSearch/WebFetch are off that flow; note for queries about katana itself |
| 5 | Image made through Bash, never shown, never asked | 10307-10311 | the Render contract lived only in a long tool description; after the Bash detour no state said "show it and ask" | built: image flow `wait -> deliver (SendUserFile) -> show` |
| 6 | Interactive command restarted and stopped instead of answered | 10284-10295 (`xtool setup` x6, TaskStop x5) | "moved to the background" gave no next step; nothing blocked a second copy of a running command | question detection was built during the day; built: terminal flow, duplicate-command and stop-while-waiting deviations |
| 7 | `SendInput` to "task 1983" | 10243-10245 | `task_id` took the first digits found anywhere; the model passed the command, which contained an e-mail | fixed: only a task number is accepted |
| 8 | A password typed by the model into a command, stored in the log | 10107 and seven more | the action log wrote arguments as they came | fixed: secret values masked before logging (old lines are still in the file) |
| 9 | Password asked through `AskUserQuestion` with one option, 52 minutes lost | 10241-10242 | the error did not say where a secret is asked | fixed: the error points to `SendInput(ask_user=true)` |
| 10 | 40 calls of trial and error on one archive, with no memory of what was learned | 10016-10048 | no place holds "this error was solved by that form" | built, off: lessons mined from failure-then-success (section 5) |
| 11 | The pre-LLM layer read "create an image of a flying car" as a code search | `uioe.log`, last request: `kind locate, code`, gate G6, no packet | Classify has kinds of *questions*; a request to *do* something has no kind | built: the flow classifier runs beside UIOE; folding both into one is open (section 7) |
| 12 | A plan "completed" by marking failed steps done | 10157-10163 | `done=N` is taken on trust | open: tie `done=N` to evidence gained in that step (section 7) |

The common root of 2 to 6: **the engine reacted to symptoms inside one request and held no state about the job.**
Each reactive note added that day is correct and local. None of them knows what step comes next.

## 2. Playbooks: state machines declared in a table

A playbook is data: states (name, the next step in one imperative sentence, the tools expected there), transitions
keyed by **event**, and a short list of tools that have no place in it. `katana flow list` prints all of it.

The event comes from what a tool **returned**, never from what the model says it did. `fl_event` maps
(tool, arguments, result) to one of 38 events through the tool contract table (section 2.2).

| Playbook | States | Entered by | Off the flow |
|---|---|---|---|
| `image` | brief, show, deliver, wait, final, failed | request words; any `Render` call | WebSearch, WebFetch, Write, Edit; a shell command that is not the render |
| `terminal` | answer, watch, ended | a result with `[waiting for input`, or a command moved to the background | WebSearch, WebFetch; stopping a task that waits; a second copy of a running command |
| `debug` | reproduce, locate, fix, verify, report, stuck | request words | none |
| `code` | read, edit, verify, repair, report | request words; a write in a folder that has a build file | none |
| `web` | search, read, answer | request words | none (cap: 6 calls or 3 dead ends, then `answer`) |
| `plan` | plan, step, report | `TodoWrite`, `Skill` | none |
| `delegate` | brief, use, redo | `Agent`, `Workflow` | none |
| `worktree` | inside, left | `EnterWorktree` | none |
| `planmode` | explore, decided | `EnterPlanMode` | Write, Edit, NotebookEdit |
| `schedule` | create, confirm | request words; `CronCreate`, `ScheduleWakeup` | none |
| `mcp` | list, read, use | `ListMcpResourcesTool` | none |
| `review` | scope, check, report, done | request words | Write, Edit, NotebookEdit |
| `deliver` | hand, tell | request words | none |

Error handling is part of each table, not a separate layer: `image` has `failed` (stop, do not try another route),
`debug` has `stuck` after three verify failures, `code` has `repair`, `web` has the cap, `delegate` has `redo`.

### 2.1 State per conversation, as a stack

State belongs to the **conversation**, not to the request (`rr_`/`rp_` stay per request). It is a stack of at most
four flows: a command that asks a question pushes `terminal` over `debug`; when the task ends, `debug` is back. A
new user message re-classifies: a clear new intent replaces the flow; "continue", "yes" keep it; anything else ends a
flow that was already at its answer state. In `image` at `show`, the next message is read as approval (to `final`),
a change (back to `brief`), or another subject (flow ends). State is lost on resume of a saved session (open).

### 2.2 Tool coverage: all 40 tools have a contract

`katana flow tools` prints the table and fails if a tool of `src/tools/table.c` has none.

| Tools | Decision |
|---|---|
| Render | own playbook (`image`) |
| Bash, PowerShell | events of `debug`, `code`, `review`; they open `terminal` |
| SendInput, Monitor, TaskStop | part of `terminal` |
| Read, Grep, Find, Symbols, LSP | evidence events (`read`) of `debug`, `code`, `review`, `delegate` |
| Write, Edit, NotebookEdit | change event of `code` and `debug`; open the verify step |
| WebSearch, WebFetch | `web` |
| TodoWrite, Skill | `plan` (a skill's steps become the plan) |
| Agent, Workflow, SendMessage, ListAgents, SubagentHandback | `delegate` (the last three: contract only) |
| EnterWorktree, ExitWorktree | own playbook (`worktree`) |
| EnterPlanMode, ExitPlanMode | own playbook (`planmode`) |
| CronCreate, ScheduleWakeup, CronList, CronDelete | `schedule` (CronDelete: contract only) |
| ListMcpResourcesTool, ReadMcpResourceTool, WaitForMcpServers | `mcp` |
| ReportFindings | last step of `review` |
| SendUserFile | `deliver`, and the `deliver` state of `image` (the desktop shows the file) |
| PushNotification, EndConversation | contract only: once, at the end, when the user is away or asked to end |
| AskUserQuestion | contract only: a decision in any flow; never a password (that is `SendInput ask_user`) |
| ToolSearch | neutral: never a step, never counted as progress or as a stall |

Finding recorded with the owner: `Render`'s own result already puts the picture on screen; `SendUserFile` is the
route for a picture (or any file) produced another way. The `image` flow says which one applies.

## 3. The per-turn input

### 3.1 The line

After every tool result the loop calls `flow_after`. The state advances, and when there is something to say **one**
line is appended to the end of that result:

    [katana: next image 2/6 show: the image is on screen from the Render result. Answer now: give the path and ask if it is approved. Call no tool]

Form: `[katana: next <flow> <i>/<n> <state>: <clauses><next step>]`; without a flow, `[katana: next: <clauses>]`.
The same line closes the user message when a request opens or continues a flow. Clauses come before the step:

| Clause | When |
|---|---|
| `off the flow: <tool> has no part in the <flow> flow.` | the call was in the playbook's off list (or one of the three terminal/image deviations) |
| `you made this exact call in an earlier request ... (N times now)` | same call and same result, seen in a previous request of this conversation |
| `the last 5 calls brought nothing new ...` | stall by evidence gain (3.4) |
| `8 calls since the plan last moved.` | open plan, no `done=N` |
| `N web calls in this flow (M gave nothing) ...` | web cap reached |
| `a failure like this one was solved N time(s) before ... by: <call>` | a mined lesson matches (off by default) |

### 3.2 When it is emitted, and the budget

Not on every result. It is emitted when the state changed, the call failed inside a flow, any clause fired, or four
results passed since the last line (the old one has scrolled away). The `plan` flow stays silent unless a clause
fires, because the `TodoWrite` result already names the step; a periodic plan reminder was cut on 2026-11-23 for
being ignored and stale, and this does not bring it back.

Budget: one line per result, typically 150 to 300 bytes (about 40 to 80 tokens), hard cap about 1 600 bytes with
every clause. It is an ORDER note, so it is outside the INFO budget of `knote.c`. When the request is in its closing
round the state still advances but no line is written (the `rounds` note already says "answer now").

**Prefix cache.** The line is only ever appended at the tail of the context (end of a tool result, end of the user
message). No earlier byte changes, the system prompt does not grow, and no tool declaration changes. A stale line
is never edited: the next one supersedes it.

### 3.3 Off-flow calls

`flow_before` runs before the tool. By default the call still runs and the clause goes in the line. With
`KATANA_FLOW=strict` the call is refused before running (`error: refused, NOT run. ... Next: ...`). The advice about
searching the web for katana itself never refuses.

### 3.4 Stall: evidence gained per step, not repetition

Hash-based repeat detection misses the loop that varies its arguments (`find` with five patterns, a login with
three flag orders). A step **gains** if any of these holds: the result of a successful call was never seen in this
conversation; the error signature (first line, digits removed) is new; a file changed; a plan step closed; the flow
state moved; the user answered. Five steps in a row with no gain raise the clause. It needs no model signal.

## 4. Retrieval guided by the state

All of this is **off by default** (`KATANA_FLOW_RETRIEVE=1`, `KATANA_FLOW_RERANK=1`).

Usual RAG asks "what text is similar to the question?" once, before generation. Here the question is not text at
all: the flow state names a **slot** that is missing, and the engine fills exactly that slot when the state is
entered.

| State entered | Slot | How it is filled | Search? |
|---|---|---|---|
| `debug/locate`, `code/repair`, error cites `file:line` that exists | the code at the failure | the engine reads lines -4..+4 | none |
| same, no location | where the error's most specific identifier appears | one literal Grep, top 4 | lexical |
| `web/read` | which result to open | the result titles, ordered by the probe | none (probe only) |
| `code/verify` | how this folder is verified | the build file that exists (`build.sh`, `Makefile`, ...) | none |
| `terminal/answer` | the value the command asks for | `[hints]` built by the terminal layer | none |
| any failure | how it was solved before | lesson table (section 5) | exact key |

The evidence block (`[katana: evidence for the next step: ...]`) is written before the `next` line, so the line that
tells the model what to do is the last thing it reads.

**Ranking.** Candidates are produced lexically and by the code graph that UIOE already has. Embeddings stay the
tie-break of `docs/embedding-usage.md` (D6): they are not called when the words already answered. Nothing here adds
an embedding call.

**Reranking by the model itself, on the cached prefix** (`fm_probe_rank`). Up to nine candidates are written after
the conversation as `[1] ... [9]`, the prompt ends at `[`, and one batch forward gives the logits of the digit
tokens: a full ranking from one pass, no generation. The same pass with empty candidates is subtracted (the model's
prior for each digit). A margin under 0.5 is reported as "no signal" and the lexical order stands. The main context
is parked and restored with `pfx_save`/`pfx_load`, the mechanism `delegate.c` uses, so the conversation is untouched.
Cost not measured: two short prefills plus one save and two loads of the context.

**Evidence graph.** UIOE builds one per request (`evgraph.c`). The flow adds the conversation-level part in a small
form: the set of result and error signatures already seen, the first error, the file it points at, the file an
image command writes. Persisting verified answer paths across sessions (beyond-rag 2.6) is designed, not built.

## 5. Learning playbooks from "failure followed by success"

`katana flow learn [log] [--days N] [--print]` reads the action log with no model and writes two tables:

- `~/.katana/flow-lessons.tsv`: (tool, subject, error signature) -> the call that worked. A lesson is the first
  successful call of the **same tool and same program** within 12 calls and 30 minutes of the failure. For shell
  commands the fix must share at least half its words with the failed command (another subcommand is not a fix).
  For other tools the lesson is the argument **shape** that worked (`{done}` instead of `{todos}`), never the values.
  Help and version calls are excluded. Secrets are masked.
- `~/.katana/flow-transitions.tsv`: (previous tool, outcome) -> next tool, with how often the next call succeeded.
  This is the raw material for mined playbook edges; nothing reads it yet.

`katana flow lessons` prints what would be used. In the loop a lesson is shown only with `KATANA_FLOW_LESSONS=N`
(N = minimum times seen) and only on a failure with the same key. On the current log the miner finds 68 lessons from 101 pairs;
many are noise (the old log truncates arguments), which is why this is off and reviewed by the owner first.
These are engine tables, not training text.

## 6. What is different from the usual RAG, and why it should be better

| Usual | Here | Why it should be better (not measured) |
|---|---|---|
| retrieve once, before generating | retrieve when a state is entered | the state knows what is missing; the first retrieval cannot |
| the query is the user's sentence | the query is a slot (an error line, a `file:line`, a prompt of a terminal) | exact keys beat similarity when the key exists; half the slots need no search |
| rerank with a second model | rerank with the same model on the prefix it already has, one forward, calibrated | no second model in memory; the ranking is conditioned on the whole conversation |
| playbook text in the system prompt | playbook in the engine; one line at the tail only when there is something to say | no standing context cost, no cache invalidation, and the step shown is always the current one |
| loop detection by repeated hash | stall by evidence gained per step | catches the loop that varies its arguments |
| memory = notes written by a model | lessons counted from the log, keyed by error signature | no model in the loop, counts only grow, nothing is rewritten |
| tool errors after the call | off-flow tools masked in the call grammar (`KATANA_FLOW_GRAMMAR=1`) | costs one resampled token instead of one loop turn |

Changes made against the first design of this work, and why:

- **Events, not self-report.** The first sketch advanced the state when the model called the expected tool. The
  state now advances only on what the tool returned (the file exists, the exit code, `[waiting for input`).
- **A stack, not one flow.** The terminal failure happened *inside* an install job; a single flow would have lost it.
- **Stall by gain** replaced the planned "N identical calls" counter (beyond-rag 5.3).
- **One-pass logit probe** replaced "ask the model to rank and parse its answer": no generation, no parse errors.
- **Tool declarations just in time** (`KATANA_FLOW_JIT=1`): when a state expects a tool outside the short prompt
  list, the declaration is appended once, saving the `ToolSearch` turn seen at lines 10246 and 10297.

## 7. Switches, and what is not built

| Variable | Default | Effect | Why off |
|---|---|---|---|
| (none) | on | state, `next` line, deviation clauses, cross-request repeat, stall | the base |
| `KATANA_FLOW=strict` | off | off-flow calls are refused before running | a wrong classification would block a valid call |
| `KATANA_FLOW_GRAMMAR=1` | off | off-flow tool names leave the grammar's name mask | forcing another name can produce a call the model did not mean |
| `KATANA_FLOW_JIT=1` | off | declaration of the expected tool appended once | costs up to ~700 tokens per tool; trade not measured |
| `KATANA_FLOW_RETRIEVE=1` | off | evidence block on entering a state | adds text to results; usefulness not measured |
| `KATANA_FLOW_RERANK=1` | off | the logit probe | runs the model; parks the context on disk |
| `KATANA_FLOW_LESSONS=N` | off | mined lessons in the line | tables are small and partly noisy |

Trace: `~/.katana/flow.log` (enter, step, end, refuse, stall, lesson, probe, retrieve, jit), one line each.

Designed, not built (order follows beyond-rag section 7):

1. **Values grounded in the index** in the call grammar (paths that exist, known symbol names) and the rejected
   probability mass as a signal.
2. **In-RAM trunk checkpoints and rollback of a failed attempt.** Rollback makes the model's context diverge from
   the visible transcript: this is an owner decision and must not be on by default. `PfxHdr`/`pfx_ssm_bytes` size
   the checkpoint; the probe above uses the disk path only.
3. **Evidence memory** persisted across sessions with hash invalidation; branch reads and branch sub-agents;
   index-constrained generative retrieval; playbook drafts through the speculative path; grounded answer decoding.
4. **Mined edges into the playbooks** (the transitions table as a prior), and pre-dispatch repair of the
   unambiguous lessons.
5. **One classifier.** The flow classifier is a word list beside UIOE's Classify; a request to *do* something
   should become a UIOE kind, and the flow state should be visible in the context packet.
6. **`done=N` checked against evidence** (failure 12), flow state saved with the session, the Gemma 4 and harmony
   result paths (only the ChatML loop path is hooked), Windows (the task checks are POSIX only).
7. **Training.** The `next` line is a new note: the model has not been trained on it. `katana-notes.md` lists it;
   a curriculum family for following it is not written.

To see it without a model: `katana flow list`, `katana flow tools`, `katana flow classify "<request>"`,
`katana flow sim <script>` (lines `U<TAB>request` and `C<TAB>tool<TAB>json args<TAB>result`).
