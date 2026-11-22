# Conventions as training data: `katana curriculum`

Goal (owner, 2026-11-21): any model placed in katana should learn three things. It should read the project file and
follow it. It should understand a skill and run it. It should use a connected MCP server. All three formats are short
and direct by design:

- **`KATANA.md`, format "brief".** One line per command (`build:`, `test:`, `test[label]:`, `run:`, `lint:`), plus
  `layout:`, `rules:`, `never:`/`ask:` (each item may end in a backtick glob the engine enforces), `done:`, `style:` and
  `docs:` (files to read on demand).
- **`SKILL.md`, format "ability".** A header with `name`, `when`, `needs` and `args`, then numbered `steps` written as
  `action => check`, then `done`, `report` and `fail`.
- **MCP card.** One line per tool: `server__tool(arg, n:number, opt?:string[], e:(a|b)) - description`, then the
  `first:`, `order:` and `avoid:` lines.

`katana curriculum` turns these conventions into synthetic conversations in which the assistant does the correct
thing. It is written in C (`src/app/curriculum.c`) and uses no model.

## The engine writes the text (2026-11-22)

Every conversation is produced inside a real project folder that the generator builds in a temporary directory: the
project files, `KATANA.md`, one `SKILL.md` per listed skill, the usage file of each MCP server, a `.git` folder, and
stand-in programs for the project's toolchain and for the skills' `needs`. The generator then runs the engine's own
code in that folder, so the text in an example is the engine's text, not a copy of it:

| text in the example | where it comes from |
|---|---|
| system prompt (rules, environment block, skills line, project block, memory absent) | `agent_build_sys` |
| MCP cards | `mcp_sys` over the scenario's servers, registered in the client table without a process |
| results of `Read`, `Write`, `Edit`, `Grep`, `TodoWrite`, `Skill`, `ToolSearch` | the tool runs (`ct_dispatch`) |
| results of `ls` and of `katana ...` commands (`brief check`, `skill check`, `mcp add`, `mcp card`) | `Bash` runs them; `mcp card` talks to a small real MCP server written into the project |
| `never:` refusal, `ask:` refusal | `brief_guard` |
| end-of-request verification note | `brief_end_note` |
| plan notes appended to results, reminder of unfinished steps | `loop_plan_note`, `loop_todo_nudge` (`src/agent/loop.c`) |
| wrapper of an MCP result | `mcp_wrap_result` (`src/agent/mcp.c`) |
| output of the project's own commands (`make test`, ...) and the data an MCP server returns | the scenario tables (the only simulated text) |

A scenario the engine does not confirm is dropped and another one of the same family is drawn. The command prints how
many were dropped and why. Reasons seen: the engine's syntax check rejects the generated code, an MCP argument value is
not in the request or in an earlier result, the engine's linter complains about the scenario's `KATANA.md`, the engine
would have demanded a plan, a `never:` rule catches a trained command, the situation repeats one already written.

The temporary folder prefix is removed from the text, so paths read `/home/<user>/...`. The operating-system line of
the environment block is the one of the machine that generates the data. The date is the scenario's.

Behaviour rules that follow from the engine and that every example now obeys:

- A request that changed files in a project with `test:` runs that command (or the backticked `done:` command) before the
  final answer; otherwise the engine sends its note, and the example shows the note and the reaction.
- An `ask:` item is asked by the engine when the command runs. The model runs the command; on "no" it gets the engine's
  refusal and stops. It does not ask first with `AskUserQuestion`.
- Plan progress (`TodoWrite done=N`) goes in the same turn as the next action; the engine closes the last step. A final
  answer with more than one step open gets the engine's reminder, and the example shows it.
- When a skill reports `MISSING:` the engine writes no plan; the model writes its own for the steps it can do.
- An optional MCP argument appears only when the request mentions it.

## Composed scenarios (2026-11-22)

The families whose names start with `x-` come from a second generator (`src/app/scenario.c`, `scenario_tasks.c`,
`scenario_more.c`). It does not fill moulds. Each conversation is one cell of a product, and a policy written in C decides
every step from the real result of the previous tool call:

- **Project.** A small project that really runs, in one of five stacks found on the generating machine (Python, Node,
  shell, C with make, Perl): two or three modules, 2 to 5 functions each drawn from a pool of 48 (whole-number
  arithmetic, so the same function is correct in the five languages), a test runner with two cases per function, and
  optionally `README.md`, `KATANA.md`, `docs/functions.md`, `settings.conf` with its check script, and task files.
- **Task** (`x-fix`, `x-add`, `x-rename`, `x-find`, `x-explain`, `x-run`, `x-config`, `x-clean`, `x-investigate`).
- **Obstacle**: none, a path that does not exist, a second failure that only shows after the first fix, two failures in
  one file (one `Edit` with `edits`), a similar name, long output, an engine error (`Edit` without `replace_all`), a
  `never:` rule, an `ask:` rule answered no or yes, a missing program, a missing file, a value the project check rejects,
  a question that must be asked, and its twin where the same request must not be asked.
- **How the request points** (nothing, a function name, a file path), **a request before** the task (find or explain; in
  `x-fix`, "run the tests", after which the model reports and stops), and **a second request** (run again, explain, find,
  re-read an unchanged file, re-read a file the user changed, bring back a result removed from the context).
- **Conventions present**: nothing, a `README.md`, a `KATANA.md` with `test:` and `layout:`, a full `KATANA.md` (rules,
  `done:`, `docs:`), plus distractor skills and MCP cards.

Every call goes through the agent loop's dispatcher (`agent_tool_run`), so the test output, the `Grep` list, the `Read`
"has not changed since" note, the "only the changed part is shown" note, the plan notes, the `never:` refusal, the `ask:`
question, the "removed to save context" note (`ah_mask`) and the stored copy that `Read` brings back are the engine's own
text. A run is kept only when it ends verified: the project's tests pass after the change, the settings check passes, the
example given in the request is a passing test case, or the correct refusal or question was produced. Otherwise it is
dropped and counted.

Other `x-` families: `x-web` (`WebSearch` and `WebFetch` against pages of an invented library served by a local server,
with fixed search results), `x-concept` (questions about katana answered from `katana --help`, `katana help`,
`katana brief show`, `katana brief check`, `katana skill list` and the tool descriptions returned by `ToolSearch`, and what
to do when something cannot be done), `x-tools` (tools listed by name only: `ToolSearch select:Name`, then the call) and
`x-skill` (a real skill of the project whose steps run).

**Questions.** `AskUserQuestion` appears in three situations and each has a twin without the question: two keys match the
request (`connect_timeout` and `read_timeout`) against one key; a fact that `Grep` does not find in the project (the
reserved port) against the same fact in `docs/ports.md`; "tidy up tools/" against "delete these two files". Each line
carries the proof (`"ask"`), and `check` verifies it on the text that precedes the question or the first change: what had
to be present is present, what had to be absent is absent.

**Check.** A line of these families carries `"scn"` (the random state, language, prompt mode and variant). `curriculum
check` rebuilds the project, runs the whole conversation again through the engine and compares the result with the line,
byte for byte. What changes between runs is normalised when the line is written: the temporary folder, the process id in
background output files, the folder of stored results, the port of the local server and the network times.

**Structural holdout.** With `--holdout` the `x-` families write only cells that the training file never has: one in four
combinations of task, obstacle, pointer, second request and convention class is reserved, and the holdout also puts a
request before and a request after the task in the same conversation, which the training file never does. Values are
reserved too (function pool, file names, library names).

**Prompt modes.** In `blocks` mode these families keep the whole environment block of the engine's prompt (working
folder, files around, skills line), because the calls use absolute paths that come from it.

`katana curriculum windows <model.gguf> <imported.jsonl> [--seq N] [--window dense --win-ctx N]` prints the windows the
training would cut, with the model's tokenizer. Only the vocabulary is loaded, so it takes about two seconds.

**Every tool the prompt names** (`x-rare`, owner's decision of 2026-11-22). The tools that the other families do not
reach get three or four situations each, 68 in a table (`src/app/scenario_rare.c`): the right use with the arguments found through
`ToolSearch select:Name`, a second use, and a case where the tool fails or is not the right one and the model reacts. They
come out in rotation, so none is missed, and `check --coverage` fails when a tool of the table is below its minimum (25
conversations for the declared tools in a file of 500 or more, 5 for every other tool, 15 conversations of `.ps1` scripts;
`check --coverage --min 2` is the form for a validation file: 2 per tool and 4 of `.ps1`).
Everything runs in the scenario's own home folder. Where a real run is impossible or would do harm, the engine's own
validation or refusal path runs, or a stand-in of the outside program, and the cell is marked `standin`:

| tool | what runs |
|---|---|
| `ScheduleWakeup`, `CronCreate`, `CronList`, `CronDelete` | the real scheduler with a fixed clock (`KATANA_SCHED_NOW`) |
| `EndConversation`, `ListAgents`, `read_log`, `howto` | the real tool |
| `TaskCreate`, `TaskUpdate`, `TaskList`, `TaskGet` | the real tools, only when the user asks for numbered tasks with details or names the tool, found through `ToolSearch`; the model's own plan stays `TodoWrite`. The tasks stay open, so the engine's reminder of unfinished steps appears and the answer is repeated |
| `PowerShell` | the real tool, which answers that it is not installed on this machine; the model says so and uses `Bash`. Not counted by `--coverage` |
| `PushNotification`, `ReportFindings` | the real tool with a stand-in `notify-send` first on `PATH` and the interface marker (`KATANA_UI`) |
| `install_package` | the real tool; `python3 -m pip` and `npm` are stand-ins that install nothing |
| `Workflow` | the real tool with the engine's test sub-agent (`KATANA_AGENT_FAKE=1`), plus two missing-file errors |
| `SendMessage`, `ReadMcpResourceTool`, `WaitForMcpServers` | only the paths with no recipient and no server |
| `SubagentHandback` | only the engine's refusal for the main agent |
| `Agent` | only the validation of the prompt: a real sub-agent needs a model (`delegate` works only inside the agent loop) |
| `terminal` | `list`, and `read` and `close` of a session that does not exist; an open session's output depends on timing |

Still out: a successful `Agent` run and a `SubagentHandback` from inside a sub-agent (both need a model), a
`ReadMcpResourceTool` that returns a resource (needs a connected server process) and an open `terminal` session.

**PowerShell scripts** (`x-ps1`, `src/app/scenario_ps1.c`). Six situations: a new `.ps1` (`Write`), a change to an existing
one (`Read`, one `Edit` with `edits`), an error message the user pasted, a pair `.sh` and `.ps1` that must stay equal, a
`KATANA.md` rule that asks for both forms, and a request where the shell script is the right answer. The model checks with
`command -v pwsh powershell` and, as there is no PowerShell on the generating machine, the final message says the script
was written and not executed here and how to run it. Every `.ps1` left on disk passes a structural checker
(`x_ps1_check`: braces, parentheses and quotes closed, `param(` first, functions named Verb-Noun with an approved verb,
no shell syntax, the expected cmdlets present); a scenario whose script fails it is dropped, and since `check` runs the
whole conversation again, the checker runs in `check` too. The shell twin, when there is one, really runs.

### Measured: the v3 file of 2026-11-22

How it was made: a pool of 1,150
conversations (`--system mix:blocks=88,real=12`), from which `katana curriculum pick --n 436` chose by coverage, and a
170-conversation validation file of new moulds (`--holdout`, checked with `check --coverage --min 2`). Windows are counted by `katana curriculum windows` with the Qwen3.6 tokenizer, after
`katana import --system keep --mask-prefill` with `KATANA_THINK=0`. Hours use 13.9 s per 512-token window. The v2 column
is the 520-conversation file of the same day, measured with the same scripts.

| | v2 | v3 |
|---|---|---|
| conversations, trained turns | 520, 1,790 | 436, 1,900 |
| distinct tool sequences (family ignored; requests separated) | 26 | 170 |
| top-20 turn templates, share of turns (digits and project folder masked) | 12.8% | 8.5% |
| families with one tool sequence | 9 | 5 ['brief-write', 'mcp-add', 'mcp-chain', 'mcp-schema', 'skill-write'] |
| families with one first tool | 15 | 9 ['brief-write', 'mcp-add', 'mcp-call', 'mcp-chain', 'mcp-schema', 'skill-fail', 'skill-match', 'skill-missing-need', 'skill-write'] |
| conversations with `AskUserQuestion` | 0 | 19 (4.4%) |
| validation conversations whose tool sequence the training file does not have | 0 of 50 | 138 of 170 (81.2%) |
| holdout turns equal to a training turn (exact text) | 27 of 179 (15.1%) | 270 of 1061 (25.45%) |
| holdout turns whose template is in the training file | 39 of 179 (21.8%) | 361 of 1061 (34.0%) |
| tokens, target tokens | 509,310, 84,120 | 528,326, 90,479 |
| windows of 512, fixed | 918 (3.5 h) | 1,003 (3.9 h) |
| windows of 512, dense with `--win-ctx 256` | not measured | 775 (3.0 h) |
| windows of 1,024, fixed | 601 | 611 |

`None`

The holdout turns that equal a training turn are calls with no free value (`ToolSearch select:Name`, a `Glob` for the test
files, the settings check of the same key). The families that still have one sequence or one first tool are the older
mould families (MCP calls, skill runs, file creation): their requests always start with the same call. They were kept for
the MCP cards and for `Write`; moving them to the composed generator is the next step.

Tool coverage of the training file (conversations with a trained call, and the minimum of the tier):

| tier | tool | conversations | minimum |
|---|---|---|---|
| A | `Bash` | 197 | 8 |
| A | `Read` | 126 | 8 |
| A | `Edit` | 74 | 8 |
| A | `Write` | 53 | 8 |
| A | `Grep` | 67 | 8 |
| A | `Glob` | 40 | 8 |
| A | `TodoWrite` | 166 | 8 |
| A | `AskUserQuestion` | 19 | 8 |
| A | `ToolSearch` | 133 | 8 |
| A | `WebSearch` | 25 | 8 |
| A | `WebFetch` | 25 | 8 |
| A | `Skill` | 42 | 8 |
| B | `git` | 5 | 5 |
| B | `recall` | 5 | 5 |
| B | `remember` | 5 | 5 |
| B | `download_file` | 5 | 5 |
| B | `terminal` | 5 | 5 |
| C | `install_package` | 5 | 5 |
| C | `read_log` | 5 | 5 |
| C | `howto` | 5 | 5 |
| B | `Agent` | 5 | 5 |
| B | `LSP` | 5 | 5 |
| B | `Monitor` | 5 | 5 |
| B | `TaskStop` | 6 | 5 |
| B | `EnterPlanMode` | 5 | 5 |
| B | `ExitPlanMode` | 5 | 5 |
| B | `NotebookEdit` | 5 | 5 |
| B | `SendUserFile` | 5 | 5 |
| C | `TaskCreate` | 7 | 5 |
| C | `TaskUpdate` | 5 | 5 |
| C | `TaskList` | 5 | 5 |
| C | `TaskGet` | 5 | 5 |
| B | `ListMcpResourcesTool` | 5 | 5 |
| B | `ReadMcpResourceTool` | 5 | 5 |
| B | `EnterWorktree` | 5 | 5 |
| B | `ExitWorktree` | 5 | 5 |
| C | `CronCreate` | 5 | 5 |
| C | `CronDelete` | 5 | 5 |
| C | `CronList` | 5 | 5 |
| C | `ScheduleWakeup` | 5 | 5 |
| C | `PushNotification` | 5 | 5 |
| C | `EndConversation` | 5 | 5 |
| C | `ReportFindings` | 5 | 5 |
| C | `SendMessage` | 5 | 5 |
| C | `ListAgents` | 5 | 5 |
| C | `SubagentHandback` | 5 | 5 |
| C | `Workflow` | 5 | 5 |
| C | `WaitForMcpServers` | 5 | 5 |
| | MCP tools | 34 | |

## Commands

```
katana curriculum out.jsonl [--n N] [--seed S] [--families a,b=2,...] [--lang pt,en] [--holdout H.jsonl]
                            [--system full|real|blocks|mix:full=F,blocks=B,real=R] [--decl PCT]
katana curriculum stats out.jsonl      # conversations, model turns, characters to learn, 512-token windows (estimate)
katana curriculum check out.jsonl [--all]   # the engine re-does what it can and checks the rest (--all: every reason)
katana curriculum pick pool.jsonl out.jsonl --n N   # chooses by coverage: tool minimums, one of each tool sequence, unseen cells
katana curriculum windows <model.gguf> data.jsonl [--seq N] [--window dense --win-ctx N]   # training windows, model tokenizer
katana import <model.gguf> out.jsonl data.jsonl --system keep [--mask-prefill]
katana specialize <model.gguf> all ... --curriculum out.jsonl     # source added by the specialize owner
```

Each line has the form `{"messages": [...], "source": "curriculum", "family": "<name>"}`.

System prompt modes (Qwen3.6 tokenizer, measured on 360 conversations; the rest of a conversation is about 405 tokens):

| mode | system message | tokens per conversation |
|---|---|---|
| `blocks` | only the scenario blocks cut out of the engine's prompt (MCP cards, skills line, project block) | 213 |
| `full` (default) | the engine's prompt without the tools section, as a model specialized on the tools sees it | 1,097 |
| `real` | the engine's prompt with the tools section, as the base model sees it | 2,388 |
| `mix:full=F,blocks=B,real=R` | one of the three per conversation, by weight | weighted |

`--decl PCT` gives the tools section to that share of the `full` conversations. Import with `--system keep`.

**`--mask-prefill` (import).** The start of every assistant turn that the engine itself writes (`<think>\n`, or the
empty reasoning block when `KATANA_THINK=0`) is left out of the loss. Import with the same `KATANA_THINK` as the model
will be served with.

**`train: false` messages.** A few assistant messages carry `"train": false`: a call that breaks a `never:` item before
the engine refuses it, a raw id before the server's error, and a final message sent before the engine's verification
note. They stay in the conversation as context and are not trained.

**`--holdout`.** Writes max(50, N/10) conversations built only from reserved slot values: the last quarter of every
content pool and of every request list, 4 MCP servers and 4 skills. The templates and the tool sequences are the same as
in the training file, so the holdout measures generalisation to new values, not to new situations.

## Families (one short example each)

| family | what the assistant does | example |
|---|---|---|
| `brief-command` | runs exactly the file's command (the labelled one when asked), then reports in words; sometimes a second request follows | "roda os testes de integration" → `Bash cargo test --test integration` |
| `brief-rule` | `layout:`/`rules:` decide where and how; when the user explicitly overrides a guideline rule, it follows the user and says so | new helper → `Write src/helpers/slug.py`; rule "tests next to the code" → two `Write`s and the test run |
| `brief-never-ask` | avoids a `never:` action and does the allowed alternative; reacts to an engine refusal without retrying; for an `ask:` item it runs the command (the engine asks the user) and stops on the engine's refusal | "commita e da push" with `never: push` → `git commit` only |
| `brief-done` | after changing files, runs `done:`/`test:` before the final message; on failure it fixes and reruns; also the engine's reminder note | `Read`, `Edit`, `Bash <done>` (fails), `Edit`, `Bash <done>` |
| `brief-docs` | reads the `docs:` file first, then follows it | "add an index" → `Read docs/migrations.md` → `sh tools/migrate.sh new ...` |
| `brief-absent` | with no file, or no such key, it looks (`ls`, `README.md`) instead of guessing | `ls` → `Read README.md` → `make test` |
| `skill-match` | calls `Skill` with the name (and `args`), runs the steps in order, marks each done with `TodoWrite`, ends with `report` | "take a snapshot of the db, call it baseline" → `Skill db-snapshot label=baseline` |
| `skill-none` | when no listed skill fits, works normally | "where is slug defined?" → `Grep` |
| `skill-missing-need` | the load result reports a missing need: it does the independent steps and says what to install | `imgcrush` missing → lists the files, skips compression |
| `skill-fail` | a check fails: follows `fail:` (a repair then the step again, or stop and report) | `tools/db.sh migrate` then the seed step again |
| `mcp-call` | `server__tool` with names and JSON types from the card; optional arguments only when the request gives them | `labelwerk__print_label {"copies": 3, "size": "large", ...}` |
| `mcp-chain` | a list call first, then the id from its result | `rowsmith__list_tables` → `rowsmith__describe {"table": "tbl-0733"}` |
| `mcp-schema` | a tool with an object argument or many arguments: `ToolSearch select:` first | `ToolSearch select:pipewright__run_pipeline` → call with `vars` |
| `mcp-none-error` | no server offers the capability, so it says so and names what is connected; on a server error it corrects an id once, or reports without retrying | "book me a flight" → words only |
| `mixed` | file, skills and servers together; the request picks the mechanism | — |

The slot grammars cover:

- 12 project stacks (C/make, Python, Node, Go, Rust, shell, a web service, a data pipeline, C++/cmake, Elixir, a
  static site, Zig);
- invented project names, built from syllables;
- 16 MCP servers with 4 to 6 tools each: an audio workstation, a ticket tracker, a home automation hub, a database
  console, a label printer, a calendar, inventory, a CI runner, mail, a photo library, recipes and vehicle telemetry
  for training; time tracking, a wiki, a music library and irrigation for the holdout;
- 16 skills;
- 8 `never:` items, 6 `ask:` items and 8 `docs:` entries;
- user requests in Portuguese and English, short, sometimes with typos.

**Rules enforced at generation time.** A scenario that breaks one is dropped and counted (see above).

- Every call to a katana tool validates against its `CtTool.props`/`req`, and the tool then really runs.
- Every MCP call validates against the schema of its card, and every value in it is in the request or in an earlier result.
- Text before a call is one short line at most.
- The engine never has to demand a plan, and never refuses a trained call.

**What `check` verifies.** It parses every line again, validates every call (MCP calls against the card parsed back out
of the system message, or against the schema that `ToolSearch` returned when the card cuts the signature), and makes the
engine redo what can be redone from the file alone:

- `TodoWrite` calls are replayed through the engine (also the plan a `Skill` result wrote); the result must be equal.
- `Write` and `Edit` are replayed in a temporary folder (the file an `Edit` changes is rebuilt from the `Read` result);
  the first line of the result must be equal.
- At each final answer the checker does what the agent loop does: the project verification must have run after a file
  change (by the rules of `brief_track`), the last open step is closed, and more than one open step needs the engine's
  reminder as the next message.
- The third call of a request without a plan is an error (the engine would demand the plan).
- An MCP result carries the engine's wrapper; a `never:` refusal quotes a pattern of the file that matches the command.
- `katana <subcommand>` in a command exists in the binary's list; the keys of the `KATANA.md` shown are keys of the brief
  format, and the file passes the engine's linter.
- No `AskUserQuestion` for something an `ask:` rule covers; no unfilled template slot (`{name}`) in a model turn.
- Two conversations with the same system prompt and the same request must start with the same action (a contradiction
  is an error); repeated situations are counted.
- The family invariants: the `brief-command` command is one of the file's; in `brief-done` the verification is the last
  command, after the last `Write`/`Edit`; in `brief-docs` the file read is listed in `docs:`; `skill-none` does not call
  `Skill` and the `skill-*` families do, with a listed name; in `mcp-chain` the id comes from an earlier result; in
  `mcp-schema`, `ToolSearch` comes before the call; every result matches a call.

Run on the file generated before this work (`--system blocks`, 1,800 conversations), the checker finds 1,163
conversations with at least one problem (first reason per conversation): 294 MCP results without the wrapper, 247 final
answers with open plan steps, 180 `TodoWrite` results the engine would not give, 172 final answers without the project
verification, 122 invented MCP arguments, 87 `Write`/`Edit` results the engine would not give, 44 double questions, 17
requests without a plan.

## Tool coverage matrix

`katana curriculum check` ends with one line per tool of the engine's table (`src/tools/table.c`), in three tiers, with the
number of conversations that call it in a trained turn and the minimum: tier A (declared in the prompt) 2% of the file and
at least 5, tier B (listed by name, reached through `ToolSearch select:Name`) 3, tier C (rare) 1. `check --coverage` turns
a tool below its minimum into an error. Measured on the 520-conversation file of 2026-11-22: 8 of the 40 tools of this
platform are called (Bash, Read, Edit, Write, Grep, TodoWrite, ToolSearch, Skill) plus the MCP tools; 32 are below the
minimum, among them Glob, AskUserQuestion, WebSearch and WebFetch of tier A. The families that would cover them are not
written yet.

## Size against the training budget

Measured with the Qwen3.6 tokenizer, fixed windows, `--mask-prefill`: 1,800 conversations in `mix:blocks=60,full=30,real=10`
are 2.17M tokens and 3,149 trained windows of 512; 520 conversations in `mix:blocks=88,real=12` are 0.51M tokens, 918
trained windows of 512 or 601 of 1024. Windows without a target (the long system prompt) are not trained.

## Structural holdout

`--holdout` alone gives the same moulds with new values. For a holdout of moulds the training file never had, generate the
training file with `--families` leaving some families out, and a holdout with only those. Measured (training without
`skill-fail`, `brief-absent`, `mcp-none-error`; holdout with only them): 54% of the holdout tool sequences are not in the
training file, 0% of the turns are equal, 22% of the turns share a template. The cost is that the training file no longer
teaches those three behaviours.

## Family weights

Without `--families` the weights are: `brief-done` 6 (the only family with the read, edit, verify, fix cycle), the `mcp-*`
families and `mixed` 4, the create families (`brief-write`, `skill-write`, `mcp-add`) 1, the others 3. With equal weights the
create families took 35% of the trained text and `mcp-call` 2% (measured).

## Measured on a file generated before the 2026-11-22 change (seed 7, 2,000 conversations)

The numbers of this section are from the older generator and equal weights. Newer numbers are in the audit report of
2026-11-22 (old against new, Qwen3.6 tokenizer).

`katana curriculum stats` reports this table. The windows columns are an **estimate**, characters / 3.3 / 512. They
count only the text that goes into the loss: assistant text plus call names and arguments.

| family | conv | model turns | chars to learn | windows* | conv/window* |
|---|---|---|---|---|---|
| brief-command | 128 | 358 | 24,792 | 14.7 | 8.7 |
| brief-rule | 148 | 463 | 84,906 | 50.3 | 2.9 |
| brief-never-ask | 118 | 252 | 32,871 | 19.5 | 6.1 |
| brief-done | 138 | 852 | 138,378 | 81.9 | 1.7 |
| brief-docs | 130 | 390 | 34,812 | 20.6 | 6.3 |
| brief-absent | 139 | 834 | 109,941 | 65.1 | 2.1 |
| skill-match | 140 | 900 | 134,634 | 79.7 | 1.8 |
| skill-none | 116 | 232 | 12,310 | 7.3 | 15.9 |
| skill-missing-need | 124 | 564 | 84,621 | 50.1 | 2.5 |
| skill-fail | 155 | 851 | 110,217 | 65.2 | 2.4 |
| mcp-call | 154 | 316 | 14,716 | 8.7 | 17.7 |
| mcp-chain | 138 | 453 | 26,865 | 15.9 | 8.7 |
| mcp-schema | 119 | 357 | 37,826 | 22.4 | 5.3 |
| mcp-none-error | 119 | 206 | 19,726 | 11.7 | 10.2 |
| mixed | 134 | 463 | 51,653 | 30.6 | 4.4 |
| total | 2000 | 7491 | 918,268 | 543.5 | 3.7 |

`katana import` measured the characters of the trained spans after rendering in each model's format: 1,276,815
(ChatML, JSON calls), 1,435,556 (ChatML, XML calls) and 1,366,436 (Gemma 4). All 2,000 conversations imported in all
three formats.

**Budget (estimate).** A night is about 100 to 150 windows of 512 tokens. At 3.7 conversations per window, one night
fits about 370 to 550 curriculum conversations if the whole night goes to the curriculum. Inside the `all` mix
(`--curriculum=20`, say), it fits about 20 to 30 windows, which is 75 to 110 conversations a night. Short families
fit 10 to 18 conversations per window (`mcp-call`, `skill-none`, `mcp-none-error`). The long ones fit about 2
(`brief-done`, `skill-match`, `skill-fail`, `brief-absent`).

**Caveat.** The system prompt, the requests and the tool results do not go into the loss, but they do occupy context.
That context is about 9.1M characters for the 2,000 conversations. The system prompt alone is about 4.5k characters
(about 1,400 tokens), which is longer than one 512-token window. How `katana train --seq 512` cuts a long conversation
decides what context the learned turns actually see. Check that before the first night. The likely fix is either
`--seq 1024+` for this source or the `--system blocks` mode.

No real-model tokenizer was run on this file. The numbers above are the estimate formula and import's character
counts.

## The measurement: `conv-*` tasks in codebench

Twelve tasks in `tools/codebench/tasks/conv-*`. Hidden tests check side effects only, never the model's words. All 12
pass `katana codebench verify`.

**Project file (5)**

| task | what passing requires | engine behaviours it depends on |
|---|---|---|
| `conv-brief-test` | `test:` is `sh tools/check.sh --suite fast`, which leaves a marker newer than the fixed file; `make test` leaves a different marker that does not count | KATANA.md in the prompt |
| `conv-brief-layout` | the new handler must be under `src/shipdesk/handlers/` per `layout:`; a same-named file anywhere else fails | KATANA.md in the prompt |
| `conv-brief-never` | the forbidden `tools/regen.sh` would produce the right result but leaves a tripwire; the fix must be made by hand | KATANA.md in the prompt; the `never:` glob enforced on Bash (without enforcement the task only measures whether the model obeys the text) |
| `conv-brief-docs` | the fact (cents, basis points, half-up rounding) is only in `docs/units.md` | KATANA.md in the prompt; the model reads `docs:` on demand |
| `conv-brief-done` | `done: sh tools/verify.sh` stamps a hash of the sources, which must match the final files | KATANA.md in the prompt; `done:` run after the last edit (the engine's done note helps) |

**Skills (4)**

| task | what passing requires | engine behaviours it depends on |
|---|---|---|
| `conv-skill-order` | 4 steps; each script refuses to run out of order; the log must be exact | skills listed as `name: when`; `Skill` loads `.katana/skills/<name>/SKILL.md` |
| `conv-skill-args` | `args: version, channel`, taken from the request into the step commands | the same |
| `conv-skill-needs` | `needs: inkpress` is missing; `missing.txt` must name it, and only the independent steps may run | the same, plus the missing-needs report |
| `conv-skill-choose` | two similar skills; only the one whose `when` fits may run | the same; the `when:` text visible |

**MCP (3)**

Each task ships a stdio `server.py` and writes a project `.katana/mcp.json`. The server logs every call.

| task | what passing requires | engine behaviours it depends on |
|---|---|---|
| `conv-mcp-call` | `print_label` with `copies` as a JSON number and `size` as an enum value | project `.katana/mcp.json` loaded; tools exposed as `server__tool`; the card shown |
| `conv-mcp-chain` | random ids; `list_tickets` first, then `close_ticket` with the right id | the same |
| `conv-mcp-schema` | 9 arguments with a nested `valve` object | the same, plus `ToolSearch select:` returning the full MCP schema |

Run the tasks on the base model and on the specialized one, alternated in the same hour:

```
katana codebench run --model M.gguf --tasks 'conv-*' --out base.jsonl --label base
katana codebench run --model M.gguf --tasks 'conv-*' --out spec.jsonl --label spec --env KATANA_SPEC=katana.kspec
katana codebench compare base.jsonl spec.jsonl
```

## What is not measured

- The data is only as good as its grammars. Nothing here shows that a model trained on it behaves better. Only the
  `conv-*` tasks before and after training, with `--repeat`, can show that. 12 tasks is a small sample, so only large
  differences mean anything.
- The code inside the generated examples is a placeholder: stubs and one-line bodies. It teaches the conventions, not
  programming.
- The output of the project's own commands and the data returned by MCP servers are still tables written by hand. They are
  the only simulated text.
- Requests, plan items, call descriptions and final answers are templates: about 12% of the trained characters. There are
  29 distinct tool sequences in 1,800 conversations; 7 families have a single one.
- Left out because the engine did not confirm them or the final answer said more than the conversation did: the `run:`
  case in the background, three `docs:` entries (config, i18n, api), one `never:` case (snapshots), and the stacks whose
  stub code fails the engine's syntax check on this machine.
- The holdout file exists, but no model has been evaluated on it.
- The data names open-source developer tools (make, cargo, npm, shellcheck...). No product, vendor or inference engine
  is named.
