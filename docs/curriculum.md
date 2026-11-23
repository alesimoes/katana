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
with fixed search results), `x-concept` (fifteen katana concepts, each in at least three situations, answered from
`katana help <word>`, `katana brief show`, `katana brief check`, `katana skill list`, `katana mcp list`, the tool
descriptions returned by `ToolSearch` and the engine's own notes; see "Katana concepts" below), `x-tools` (tools listed by name only: `ToolSearch select:Name`, then the call) and
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

**Every tool the prompt names** (`x-rare`). The table in `src/app/scenario_rare.c` has 197 situations: 174 for the training
file and 23 reserved for the validation file (`hold = 1`; the rotation of each file only walks its own rows, so a reserved
request never appears in training). A situation is a row with its own request intent and its own tool sequence or outcome.
Every tool has at least five training situations: the right use, a use combined with another tool, the engine's error and
the reaction, an error followed by another way, and for the scheduling tools the contrast between scheduling
(`ScheduleWakeup`, `CronCreate`), running now in the background (`Bash`) and watching (`Monitor`). A row can require a proof
in the last result (`need`) and name the result line the final message quotes (`quote`).
`KATANA_CU_RARE_TABLE=1 katana curriculum x.jsonl --families x-rare=1` prints the situations per tool. `check --coverage`
fails when a tool is below its minimum (25 conversations for the declared tools in a file of 500 or more, 5 for every other
tool, 15 conversations of `.ps1` scripts; `check --coverage --min 2` is the form for a validation file: 2 per tool and 4 of
`.ps1`). Everything runs in the scenario's own home folder.

What runs for real, set up by `src/app/scenario_rare2.c`:

| tool | what runs |
|---|---|
| `ListMcpResourcesTool`, `ReadMcpResourceTool`, `WaitForMcpServers` | a small stdio MCP server written into the project and started through the engine's client before the prompt is built (four resources, one binary); a second configured server whose command does not exist gives the failed and unavailable paths |
| `ListAgents`, `SendMessage` | another session registered in the mailbox of the scenario's home folder (a parked process); delivery by name and by pid |
| `Agent`, `SendMessage` to `agent-N`, `Workflow` | the engine's sub-agent hook (`KATANA_AGENT_FAKE=1`); the hook returns what the engine's `Grep` or `Glob` find in the project at that moment |
| `terminal` | sessions really opened: state kept between sends, list, close, duplicate name |
| `LSP` | a minimal language server for Python files placed first on `PATH` (workspace symbols, document symbols, definition, references) |
| `git`, `EnterWorktree`, `ExitWorktree` | a real repository with one commit |
| `Monitor`, `TaskStop`, background `Bash` | real processes; the generator waits for the output file instead of a fixed time |
| `ScheduleWakeup`, `CronCreate`, `CronList`, `CronDelete` | the real scheduler with a fixed clock (`KATANA_SCHED_NOW`) |
| `TaskCreate`, `TaskUpdate`, `TaskList`, `TaskGet` | the real tools, only when the user asks for numbered tasks or names the tool; the model's own plan stays `TodoWrite`. A recorded task no longer triggers the unfinished-steps reminder |

Still stand-ins: `PushNotification` and `ReportFindings` (interface marker and a stand-in `notify-send`; the generator sets
`KATANA_NO_NOTIFY=1`, so no real notification leaves it), `install_package` (stand-in `pip` and `npm`), `SubagentHandback`
(only the refusal for the main agent), `Agent` with an empty task. What changes between runs is normalised when the line is
written: schedule ids, the pid of the other session and of a shell session, the id of a new notebook cell, the date the
memory tool stamps. Still out: a `SubagentHandback` from inside a sub-agent (it needs the sub-agent's own loop, which needs
a model).

**PowerShell scripts** (`x-ps1`, `src/app/scenario_ps1.c`). Seventeen situations, fourteen for training and three reserved
for validation: a new script (three different scripts), a change, a new switch passed down in four edits, an explanation
without changes, a port from the shell twin, a pasted error in the script, a pasted error that comes from the call, shell
syntax inside the `.ps1`, the `.sh` and `.ps1` pair, the `KATANA.md` rule, the request where the shell script is right, and
a request to run the script, where the model looks for PowerShell, does not find it, and gets the answer another way (the
shell twin, or the same count with the shell) before saying the `.ps1` itself was not run. Every `.ps1` left on disk passes
a structural checker (`x_ps1_check`: braces, parentheses and quotes closed, `param(` first, functions named Verb-Noun with
an approved verb, no shell syntax, the expected cmdlets present); a scenario whose script fails it is dropped, and since
`check` runs the whole conversation again, the checker runs in `check` too.

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

## Version 4 (2026-11-22): conventions in the composed engine, request shapes, dependent requests

Every conversation of the v4 files comes from the composed engine and is replayed by `check`. The nine mould families
that had one tool sequence or one first tool were ported; the moulds stay in the binary but are not in the default mix.

### The three convention families (`src/app/scenario_conv.c`)

| old mould family | where it is now | situations (cell names) |
|---|---|---|
| `mcp-call` | `x-mcp` | `id-known` (direct call), `id-in-file` (the id is in a project file: `Read` first), `wrong-tool-name` (the user names a tool the server does not have), `busy-once` (the server answers busy; the same call is sent once more), `locked` (the server refuses; the list shows the item unchanged), `server-down` (two different tools fail; nothing changed; `/mcp reconnect`) |
| `mcp-chain` | `x-mcp` | `by-name` (list, then the call; a search-type list is called with the name), `bare-number` (the user gives 2231, the list gives `trk-2231`), `two-items` (one list, two calls, with a plan), plus the server's `first:` tool when the card has one |
| `mcp-schema` | `x-mcp` | `big-tool`: the card shows at most eight arguments and does not open objects, so `ToolSearch select:` comes first only when the request uses something the card does not show; otherwise the call is direct |
| (`mcp-none-error`) | `x-mcp` | `nobody-offers` (answer without calls), `server-down-export` (the server died; the same list is in an export file of the project, found with `Glob` and read, and the answer says where it came from) |
| `skill-match` | `x-ability`, `x-skill` | `similar`: `snapshot` (copy of the sources) and `backup-settings` (copy of `settings.conf`) are both in the project and the wording of the request decides; `arg-missing` (the label is asked with `AskUserQuestion`) |
| `skill-missing-need` | `x-ability` | `need-missing` (`pack-release` needs `packsign`: the engine writes no plan, the model plans and does the two steps that do not need it and says which one did not run), `all-steps-need-it` (`sign-archive`: `command -v` confirms, nothing runs) |
| `skill-fail` | `x-ability`, `x-skill` | `step2-fails` (`preflight` with a setting out of range: stop at step 2, as `fail:` says), and step 1 failing in `x-skill` |
| (new) | `x-ability` | `read-only` (what does the skill do: `Read` of `SKILL.md`, nothing runs), `where-from` (`katana skill list`) |
| `brief-write` | `x-make` | `brief-new` (find the test command, run it, `Write`, `katana brief check`), `brief-init-message` (the request is the engine's own `/init` message; the first file exists and is read before it is rewritten), `brief-add-rule` (the file exists: `Read`, `Edit`), `brief-broken` (a misspelled key; the error line is pasted by the user or comes from `katana brief check`) |
| `skill-write` | `x-make` | `skill-new`, `skill-new-message` (the engine's `/skill new` message; the template file is read first), `skill-add-step` (`Read`, `Edit`, `katana skill check`) |
| `mcp-add` | `x-make` | `mcp-add`, `mcp-add-find-path` (the user gives the wrong path: `Glob` finds the program first), `mcp-already-there` (`katana mcp list` first; nothing is added) |

**A real MCP server.** In `x-mcp` the scenario's server is a process (`python3`, written by the generator outside the
project) that the engine's MCP client starts and talks to. It keeps state: items with ids, a list tool (with a text filter
when the tool is a search), tools that record their arguments in the item, and three failure modes (busy on the first call,
one locked item, exit on every call). The target tool answers with the data of the scenario table of that server, filled
with the id it was called with; the final message uses the table's sentence for the same data and quotes the reply. Every
argument value must be in the request or in an earlier result, or the scenario is dropped. In `x-make` the server program
is in `tools/` of the project and `katana mcp add`, `katana mcp list` and `katana mcp card` talk to it.

**The user corrects the plan mid-way** (`midway`). In `brief-new` and in `x-config` the user interrupts after a tool result,
with the plan open: the engine's loop then knows a plan was open (`pl_old`), the model rewrites the list with the new step
and continues from it. This is the only place where a request arrives without a final answer before it.

### Request shapes (`src/app/scenario_shape.c`)

The family writes the request; a layer then gives it a shape drawn from the conversation's seed (the check replays it).
Nothing in a shape changes what is asked: names, paths, values and backticked text are never touched.

| shape (line field `shape`) | what it is |
|---|---|
| `plain` | the request as the family wrote it (short) |
| `ctx` | long with context: one sentence about the user's situation before or after (16 sentences per language) |
| `polite`, `blunt`, `formal` | greeting and thanks; "asap", "quick", "!!"; capital letter and full stop |
| `typo` | one or two typing errors, only in common words of a fixed list |
| `mix` | the other language in the greeting or in the context sentence |
| `lead` | second requests that start with "ok.", "thanks. now" |
| `wrong` | the user assumes something false: a path that does not exist, the wrong function blamed for a failing test (`+blame`), a tool the server does not have, a server program in the wrong folder, a wrong idea about katana (`x-concept`), a URL that returns 404 |
| `two` | two requests in one message: the short request (find, explain) and the task are joined, and the two answers come in one final message; two settings at once; two items on a server |
| `paste` | pasted text: the real output of the test command, the real error line of `katana brief check`, the binary's real error for an option that does not exist |
| `goal` | a follow-up that changes the goal: another value, undo, another name for the rename, one more rule |
| `midway` | the correction arrives in the middle of the work (above) |
| `dep` | the conversation has a second request that depends on the first (below) |
| reserved for validation: `fwd`, `ps`, `held-lead` | a request forwarded from someone else ("forwarding from the team chat: ..."), a postscript with context, second requests that start with "one more thing:", "while you are at it:"; the last five context sentences are also reserved |

### A second request that depends on the first (line field `dep`)

Definition: the second request cannot be carried out or understood without the first one, because it refers to its object
or to its result by a pronoun, by "again", "that", "the same", or corrects it. The generator sets the flag at the place
where such a request is written; `curriculum variety` counts the flag. Kinds: "fix it" after a test run that only reported;
"where is it used?", "does it have a test?" (one `Grep`), "which file was that again?" (answered from the conversation, no
call); "run them again"; "read the file again" (the engine answers that it has not changed); a result removed from the
context and read back; the goal changes; the same MCP call for another item, or the state the server shows now; the same
skill with another label; another fact of the same web page, or what else the search listed; the line that was just added;
"which tools did you use for that?" and "what did the last tool return?" (answered from the conversation; these two are
generic and kept rare: 8% of the eligible conversations, 22% in `x-rare`, `x-tools` and `x-concept`).

### Katana concepts (`x-concept`, `src/app/scenario_concept.c`)

Fifteen concepts, each in at least three situations in the training file (`pick` takes three per concept; `curriculum
matrix` prints the count per concept). Help questions run `katana help <word>`, never the whole help, so a new help line
changes a stored conversation only when it contains the searched word. The `command -v` lookups of the "cannot be done
here" situations run with a `PATH` the generator builds, so they do not depend on what the host has installed.

| concept | where in the engine | how the training file shows it |
|---|---|---|
| commands (subcommands) | `main_usage`, `lc_known_cmd` in `src/app/launch.c` | `katana help <word>` answers how; a wrong assumption (`katana http`, `katana export` to PDF) is corrected with the line; a command that does not exist is searched with two words and then reported as absent |
| options of the launcher | `lc_usage`, `launch_try` in `src/app/launch.c` | how (`-p`, `--output-format`, `--mcp-config`, `--model`); a wrong option found under its real name; the binary's real error for an unknown option, pasted by the user |
| console commands | `con_help_text` in `src/app/console.c`; `/plan`, `/go`, `/undo`, `/compact`, `/cd`; `/status`, `/model` | `katana help compact`, `/model`, `context`; a wrong name (`/readonly`, found as `/plan`); `/undo` after a real change |
| limits | tool descriptions (`src/tools/table.c`); `ct_rd_lines` in `src/tools/read.c`; cap of 100 in `src/tools/glob.c` | `ToolSearch select:` for a number (Read 2000 lines, Bash timeout, Monitor, Glob); a `Read` with `limit` returns the partial note and the next request uses `offset`; Glob shows 100 of N and the answer takes N from the note |
| sessions | `-c`, `-r`, `--session-id`, `--no-session` in `src/app/launch.c`; `/session`, `/resume` | continue, pick an older one, run without recording; the wrong idea that sessions are global |
| plan | `TodoWrite` in `src/tools/tasks.c` (`done`, refusal of an unchanged list, `ct_todo_autofinish`); `loop_plan_note`, `loop_todo_nudge` | how progress is marked; the wrong idea that the whole list is resent; a checklist where the same list is sent again (not trained), the engine refuses it, and the trained turn does the step. Every task family also plans with `done=N` |
| edits | `src/tools/edit.c`; `/undo` in `src/agent/undo.c` | the user changed the file after the read, the `Edit` gets "0 matches", the model reads again and edits the real text; `edits` and the uniqueness rule; how to undo. Also `x-rename` (the `replace_all` error) and `x-fix` (`edits` blocks) |
| read by difference | `src/agent/facts.c` ("has not changed since", "only the changed part is shown") | the user doubts a re-read and the answer quotes the engine's note; the user changed the file and asks for a line outside the changed part; why the `Read` was so short. Also the reread second requests of the task families |
| results removed from the context | `ah_mask` in `src/agent/context.c`, stored copy under `~/.katana/context` | after four requests the test output is replaced by the engine's note; the user asks for its first line, thinks it is lost, or pastes the note; each time the stored copy is read back. Also the `result-removed` second request |
| KATANA.md | `br_parse`, `brief_guard`, `brief_end_note` in `src/agent/brief.c`; `katana brief show`/`check` | what is enforced, lint, the test command the engine read. Also `x-make` (create, add a rule, fix a key) and every task family with a project file |
| skills | `src/agent/extend.c` (`katana skill list`/`path`), `src/tools/skill.c` | list; where katana looks (project first, then home); a skill the user believes exists and does not. Also `x-skill`, `x-ability`, `x-make` |
| trust | `mcp_trust_project` in `src/agent/mcp.c`; `trust_init` and `KATANA_CONFIRM` in `src/agent/tools.c`; `/cd` | a hand-written `.katana/mcp.json` is skipped as "not trusted yet" and the model explains it without setting the variable itself; `/cd` asks for trust; "always asks before a command" corrected with `KATANA_CONFIRM=1` |
| refusals | `brief_guard` (`never:`, `ask:`); `PLAN_DENY` in `src/agent/planmode.c` | a `never:` refusal and the user insists; what happens on deletes; `ask:` answered no. Also `x-clean`; plan mode in `x-tools` and `x-rare`; the main agent's `SubagentHandback` in `x-rare` |
| cannot be done here | rule in `src/agent/prompt.c` ("try another way ... say what you could not do and why") | a plan, `ToolSearch` by word (no tool), `command -v` for the usual programs (none), then what is needed: email, print, chat |
| tool discovery | `src/tools/toolsearch.c` (keyword search) | schedule, notebook, worktree by keyword, answered with the tool's own description. Also `x-tools` and `x-rare` (`select:Name`) |

Not covered: the `sudo` path (with a stand-in `sudo` on `PATH` the engine could believe a root write succeeded), a real
session listing (`-c` and `-r` need an installed model).

### Selection and measures

`katana curriculum pick` takes the conversations in this order, always preferring a conversation whose tool sequence is
not in the file yet (ceiling of three conversations per sequence, lifted only where a minimum cannot be met otherwise): the
real full prompt up to 10%; every first tool a family has in the pool; situations (five per rare tool, three per concept,
each `.ps1` situation once); the minimums of the rare tools, of `.ps1`, of MCP and of each family; the minimums of the
declared tools; the `x-ctx` quota (5% of the file, at least two thirds with the context block and the rest without it, taken even above the per-sequence ceiling: the family has few tool sequences and the ceiling alone cut it to about 1%); dependent second requests up to 22%; a minimum of each request shape; one conversation of every sequence
not seen yet, shortest first; unseen cells; the rest.

`katana curriculum variety train.jsonl validation.jsonl -v` prints every measure below, with these definitions:

- **tool sequence**: the tools of the trained turns in order; every MCP tool counts as `mcp`, a `TodoWrite` with only `done`
  as `done`; `|` separates requests and `.` is a turn without a call. The family is not part of it.
- **request**: a user message that is not an engine note. **Masked request**: backticked text, paths, file names and digits
  replaced by marks, case and spacing ignored.
- **trained turn**: an assistant message without `"train": false`, text plus calls with their arguments, compared exactly.

@@V4TABLE@@

## Version 5 (2026-11-22): MCP cards and the end-of-request note

Files that were in `/mnt/dados/cu-v5` (removed on 2026-11-22; the v6 generator and files supersede them, see `data/curriculum/v6/`; `run.sh` was the recipe: pool of 1,500 with `--system mix:blocks=88,real=12`, `pick --n 440`, 170
validation conversations). Everything is replayed by `check`: 456 training conversations (440 picked plus 16 card
conversations added from the pool, because the picker ranks by tool sequence and an answer without calls is a common one) and 170 validation conversations, 0 problems.

- **`x-mcp` `card-tools`**: "what can the X server do?" is answered from the card already in the prompt, with no call.
  **`card-args`**: "what do I have to give the tool Y?" is answered from the card (required and optional arguments), with no
  `ToolSearch` and no call. 8 and 10 conversations in the training file, 3 and 0 in validation.
- **Brief end-of-request note** (cell suffix `/end-note`). The model finishes after changing project code without running the
  `test:`/`done:` command of `KATANA.md`; that message is `"train": false`. The engine's `brief_end_note` then sends its note
  (generated by the real function, as a user message) and the trained turn runs the project's check and answers. It applies in
  `x_test_end` (`scenario_tasks.c`) wherever a task finishes with a project file present: 60% of the cases where the engine
  would send the note. Skill, MCP usage-file and `KATANA.md` edits do not trigger it (owner's rule of 2026-11-22), so `x-make`
  almost never has it. 11 conversations in training, 4 in validation.
- **Layout** (`x-find` cell suffix `/layoutN`, `x-explain` `/layout-tour`): with a `KATANA.md` that has `layout:`, "where are the tests / the source / where does a new function go" is answered from the layout with no call, and "how is the project organised" starts from it and checks the source folder with one `Glob`.
- **`x-docs`** (`src/app/scenario_docs.c`): the project has `docs/index.md` (a map: what each module does, with a link) and one document per module; `KATANA.md` has a `docs:` line pointing to the index. The engine's selector (`docsel`, see `docs/extending.md`) is the real code, run on the request during generation (it needs a model vocabulary: `KATANA_VOCAB_MODEL=file.gguf`, or the first `.gguf` of the models folder). Three situations: `hit` (the request uses the index's words; the engine attaches the right document; the model answers from it with no call), `miss` (the same thing in other words; the engine finds nothing; the model reads `docs/index.md`, then the module document, then answers), `wrong` (the engine attaches the document of another module; the model says so and goes through the index). The point is that the selector is plain code and can fail, so the model also learns to do the walk itself.
- **Not done**: the example note (`brief_example_note`: the request has examples and files changed) is not generated; the
  situations that need a real model (`Agent`, `SubagentHandback` inside a sub-agent); `/server:prompt` and `list_changed` for cards.

## Version 6 (2026-11-22): Find, several calls in one turn, one script in one call

Three composed families that teach the model what the engine gained last: the `Find` tool (`src/tools/find.c`, now in the
main tool list of the prompt, see `katana agentsys`), several independent calls in the same assistant turn, and one `Bash`
script for repetitive work. Everything is produced by the same machinery as the other `x-` families: the project is real, every
call goes through the engine's dispatcher, no result is written by hand, and `check` runs each conversation again and compares
it byte for byte. No model was run for any of this.

**Project changes** (`scenario.c`). `XProj.cmt` puts a one-line English comment above every function (what it does, in the
words of the function table), so the index has something to match. `XProj.wrap` adds `report_<f>` to one module: a function that
calls `f` of another module (a real import in Python, JavaScript, shell and C), which is what gives `Find` a `calls:` line and
`mode=refs` a caller to show. Both are off for the older families, so their stored conversations do not change. `XMsg` now holds up
to four calls per turn (`c[4]`), and `x_calls` writes one assistant turn with two to four calls (a plan progress call counts as
one), runs each through the dispatcher in order and stores one tool message per call; the sequence in the line reads `Read+Read`.
`check`, the importer and the three conversation formats already handled several calls in one turn (ids, one result per call).

**`x-find6`** (`src/app/scenario_v6.c`; stacks Python, JavaScript, shell and C: the `Find` index does not read `.pl` files, so there is
no Perl cell). 28 functions have a description in other words than the code's (`g_x6_c`: how the user says it in each language, the
short query the model sends to `Find`, the code term it tries in `Grep`). Five situations; the final answers are short and cite
`file:line`, and the line number is read from the real `Read` result.

| situation | what the conversation does |
|---|---|
| `concept` | the request describes the concept ("the price a customer pays once a percentage is taken off"); `Find` with a short query (sometimes with `path`); the first candidate must be the right file; `Read` with `offset`/`limit` around the best line; answer with `file:line`. A second request may ask who uses it (`Find mode=refs`, answer from the real "Used N times in M files") or what it returns exactly (answered from the part already read, no call) |
| `refs` | the report function `report_<f>` calls `f`; the request asks which function feeds the report and where the real calculation lives; `Find` shows the report function (the `calls:` line names `f`, or the definition lists the report file under "also referenced in" when the language has no `calls:`); `Find mode=refs` on the name; `Read` of the use or of the definition; the answer names both with `file:line`, and the tests when the refs output shows them |
| `no-match` | the user's words (up to four, taken from the request, none of them anywhere in the project) give the engine's real `No match for ...`; the model does not insist and does not guess: `Grep -i` with the code term, then (half the cases) a `Read` that confirms the definition, then the answer says why `Find` had nothing |
| `exact` | the exact name is known: `Grep` straight to `def name(` with `-A 2` (no `Find`), answer with `file:line` and the return expression from the output; in 30% a second request describes a concept and goes to `Find`, so one conversation holds both choices |
| `confuse` | two functions with common words in their comments, in different modules (ten pairs: `discount`/`with_tip`, `area`/`perimeter`, `dozens`/`tens`, `half`/`quarter`...); the query uses only the common words; the model reads the first candidate before affirming: if it is the right one it says it checked, if it is the other one (the ranking decides, the policy follows the real output) it says so and reads the second candidate, then answers with `file:line` of the right one |

**`x-batch`** (`src/app/scenario_batch.c`; all five stacks, but the two cells that use `Find` skip Perl). The independent calls come in
one turn and the next step uses the results together:

| situation | turn 1 | turn 2 |
|---|---|---|
| `reads2` | `Read` + `Read` of two modules named in the request | answer lists what each defines; a dependent follow-up asks which has more functions (no call) |
| `reads3` | `Glob` of the source folder (one call) | `Read` + `Read` + `Read` of all the modules; answer is one line per module; a follow-up asks for the biggest |
| `grep2` | `Grep` in the source folder + `Grep` in the tests folder (`path`) | answer counts the lines in each; a follow-up asks for the test lines (one `Grep`) |
| `find-glob` | `Find` + `Glob` of `**/test_*` | `Read` around the definition + `Grep` of the name in the test file, again in one turn; answer gives the definition and its test file; a follow-up runs the tests |
| `find2` | two `Find` (two questions in one request) | two `Read`, one per candidate, in one turn; the answer gives both locations |

Reads that depend on a result stay in separate turns (the `confuse` situation of `x-find6` reads the second candidate only after
seeing the first).

**`x-script`** (`src/app/scenario_script.c`; all five stacks). One `Bash` instead of one call per file, function or test; the script
is written for the stack of the project and really runs there, and the generator checks the output line by line against what the
project has.

| situation | the call |
|---|---|
| `count` | a shell loop over the source files with `grep -c`, piped to `awk` for a total; answer is the table; a follow-up runs the tests |
| `per-function` | one test run into a variable, then a loop over the function names (a `sed` pattern per language) that prints `name: ok` or `name: FAIL`; zero, one or two functions are broken on purpose; a follow-up asks for the first failing case (the project's own `FAIL` line) |
| `replace` | `Grep -c` with `\bname\b` gives the files and the count; ONE Python script replaces the name in those files, prints one line per file and `assert`s the total against the count; then the tests run; a follow-up undoes it with the same script the other way round and the same assert |
| `syntax` | a loop with the language's own check (`python3 -c 'import ast...'`, `node --check`, `sh -n`, `gcc -fsyntax-only`, `perl -c`) printing `ok file` or `BAD file`; in 40% one module is really broken |

**Where they sit in the engine.** `curriculum.c`: the three files are included after `scenario_docs.c`, the names are in
`g_cu_fams` with the other `x-` families, `Find` is a tier A tool of the coverage table (`g_cu_cov`), and the default weights are
`x-find6` 30, `x-batch` 20, `x-script` 20. `scenario_more.c`: the names are in `g_x_fams`, which is what `check` uses to replay a line.
The holdout reserves cells with `x_held` (offsets 180, 190, 200) and, through `cu_ix`, the last quarter of the function table.
`pick` needed no change: it reads the tool names of the conversation, and `Find` is now one of them.

**Generated for this step** (not a training file: a pool only to make `check` pass; seed 6, `--system mix:blocks=88,real=12`):
300 conversations, 134 `x-find6`, 76 `x-batch`, 90 `x-script`; 129 call `Find` and 120 of them start with it; 76 have two or more calls in
one turn; 30 distinct tool sequences; `check`: 771 calls, 0 problems, 0 contradictions, 0 repeated situations, all 300 replayed by the
engine. A holdout of 50 (`--holdout`) also checks clean. The generator dropped about 1% of the draws (a function that landed in the
module where it must not be, a case set the pool could not build).

**Not verified.** No model was run, no training and no measurement was done, so nothing here shows that a model trained on these
conversations uses `Find`, batches its calls or writes scripts. The query words and the "other words" descriptions are written by
hand (28 functions); the model's own query would differ, and the generator keeps only the cases where the real index agrees with the
policy, so the examples show `Find` working and never `Find` ranking the right file low except in `confuse`. The `report_<f>`
function in Perl exists in the code but is never generated (no Perl cell uses `Find`). The projects are tiny (two to four functions
per module, one 40-line chunk per file), so the examples do not show `Find` choosing among many chunks of one file or a large index.
The plan rule in `loop.c` counts `Find` as a call that is not a read, so a request with three non-read calls would be asked for a plan;
these families stay under that, but a real session that chains `Find`s would not (see the handoff).

## Version 7 (2026-11-23): Symbols, the new Find, a project in another place, the read cursor, `[katana: ...]` lines

v7 is v6 regenerated for the engine as it is now, plus five families that teach the model to USE what the engine gained since v6.
Nothing is copied from the v6 files: `data/curriculum/v7/generate.sh` rebuilds every family (v1 to v6 and the new ones) with the
binary at hand, so the system prompt of the examples is the current one (`katana agentsys`: `Symbols` on the Files line, the rule that
lines in `[katana: ...]` come from the program and must be followed, "Find first; Grep only for an exact pattern; Glob only for file
names by pattern", including the `real` prompt mode that kept the old "by meaning" wording in v6) and every tool output is the real
output of today. The model is trained with `--no-tool-decl`, so it learns the names and arguments of `Symbols` and `Find` from these data.

**What changed in the machinery** (`scenario.c`, small and off for nobody: it is the real loop's behaviour).
`x_run` now goes through the same repeat control as the agent loop (`rr_check` before the call, `rr_after` after it), and `x_user` and
`x_begin` call `rr_reset` as the loop does at each request. So an identical read repeated in one request returns the engine's repeat
note instead of the content, a tool that changes files empties the table, and the read cursor (`rc_*`) starts over at each request. The
older families only repeat a read by accident, so only a handful of their conversations changed (7 of 600 carry a repeat note, 3 the
cursor note). `g_x_planok` lets a conversation keep the engine's `[katana: plan first ...]` note (every other family still treats it as an
error). Of the old families only the wording of two of x-find6's own lines changed ("Searching by meaning first" and "The exact name is
known: Grep, no need to search by meaning" no longer match what `Find` is; the tool outputs of x-find6 were already real).

**New projects** (`scenario_v7.c`). Seven languages (Python, JavaScript, TypeScript, C, Go, Rust, Java), three to four modules, 12 to 15
functions in three layers (leaves, middle, top) with real calls between modules (`from .m import f`, `require`, `import`, `#include`
with headers, `use crate::m::f`, `Class.f(...)`), a "hub" leaf called by three functions, and for `x-find7` one or two message constants
(`QUOTA_FULL = "quota exceeded"`). The big-file variant has one module of 15 functions with eight comment lines each (200+ lines, over
the 8 KB / 200 line threshold of the read map). Everything is written to disk and the engine's index is built by the first call.
Dates, ids and the index time (`[symbol index created in 0.0 s]`) are normalised in the stored text.

**Language.** In the five new families about 10% of the requests (and their answers) are in Portuguese about English code (`x7_lang`);
the older families keep their 50/50 split. The holdout reserves cells with `x_held` (offsets 210 to 250) and the last quarter of the
choices through `cu_ix`.

| family | situations (what the conversation does) |
|---|---|
| `x-symbols` | `callers` (answer cites the call sites; 45% read only the range the output gave, `offset`/`limit` from `in FN (a-b)`; 35% a follow-up asks what breaks, answered with `impact`), `def` (file and lines; read of the range; follow-up for the callers, with the honest answer when there are none), `callees` (call order; a follow-up reads the first one), `impact` (direct and indirect callers, `depth` sometimes), `impact-none` (NOT FOUND, said as "nothing calls it that the index sees", with its limits), `at` (which function contains line N), `deps` (JavaScript, TypeScript and C only: the languages whose imports the index resolves), `parallel` (two independent `Symbols` in one turn: callers + callees of the same name, two `def`, two `outline`), `outline-read` (outline, then `Read` of only the function's range), `typo` (the engine's "No exact match ... showing X" with low confidence, reported as such), `missing` (NOT FOUND with the indexed set, no invention) |
| `x-find7` | `ident` (an identifier: "Found by name or text (checked in the files)" with `[confidence high]`; 40% read three lines; 45% a follow-up with `Find mode=refs`, which the symbol index now serves: "Defined", "Used N times in M files"), `quoted` (a quoted message: "Literal text found", answered with file, line and constant), `quoted-missing` ("NOT FOUND (literal)": a `Grep -i` of a word that is nowhere, or no call; the answer says the text is not in the project), `typo` (an identifier with a typo: "NOT FOUND: nothing is named ..." with `[confidence low]` similar names, then `Symbols def` of the closest and an answer that says it assumed that name), `concept` (a description in other words: v6's machinery with the current output; 35% follow-up with refs), `no-match` ("No match for ... try recall(query)", then `Grep` with the code term) |
| `x-place` | the user mentions a project that is not under the working folder (or only its name), the working folder being the home folder or another folder; the request arrives with the engine's `[katana: you mention "X", which does not exist from the working folder (W). It is at: P. Work there: pass that folder as path ...]` line, computed by `place_note` itself; the model passes `path` (Symbols, Find, Glob, Grep) or an absolute path from the first call: `symbols`, `find`, `glob`, `grep`, `abs-read`; `did-you-mean` (Read of a path that does not exist returns "Did you mean: P?", the model reads P); `folder-exists` ("The folder X exists at P, but lib/f is not in it": `Glob` with `path`, then `Read`); `home-refusal` (a `Find` or `Glob **/` in the home folder is refused; that first call is context only, not trained, and the model repeats it with `path`) |
| `x-cursor` | a 200-line file read in parts: `rest` (`Read` with `limit`, then `Read` without a range: "lines 1-N ... were already given to you in this request; here is the rest, from line N+1"), `chunks` (explicit offsets, no note), `same-offset` (same offset, bigger limit: the cursor note), `map` (a `Read` without a range of a big file returns its map and first 40 lines; the model then reads only the function's range), `repeat-switch` (an identical `Read` repeated: the repeat note, then `Symbols outline`), `repeat-answer` (the note, then the answer from what was read), `stuck` (three repeats: the third-time note, then an honest "I could not tell" that says what was and was not read). The repeated calls are context only (not trained) |
| `x-katananotes` | following any `[katana: ...]` line: `repeat` (a repeated `Symbols`, `Grep` or `Find mode=refs` gets the note; the model looks somewhere else: the callers of the caller), `plan-first` (three `Edit`s without a plan: the third result carries "[katana: plan first ...]"; the model writes the plan, re-queries `Symbols` (NOT FOUND: no caller left), runs a syntax check; Python, JavaScript or C), `todo-open` (the model answers with plan steps open; the engine's "your todo list still has unfinished steps" message arrives; the model closes the list with `TodoWrite done=4` and repeats the answer; the premature answer is context only) |

**Counts** (`data/curriculum/v7`, see its README): training file 1042 conversations (712 from the v1 to v6 families with v6's weights and recipe,
v6 had 697; new: `x-symbols` 110, `x-find7` 70, `x-place` 60, `x-cursor` 40, `x-katananotes` 50), validation file 239 (189 old + 50 new, after the
repeatability filter). 183 conversations call `Symbols`, 195 call `Find`, 294 have calls in parallel, 125 have the real full prompt. The new
families are 10% Portuguese (32 of 330). `katana curriculum check`: 1042 conversations, 3349 calls, 0 problems, 0 contradictions, 0 repeated
situations; validation: 239 conversations, 1141 calls, 0 problems. New default weights: `x-symbols` 40, `x-find7` 25, `x-place` 20,
`x-cursor` 15, `x-katananotes` 15 (the data files use fixed counts per family instead, see `generate.sh`). `Symbols` is in the coverage table (tier A).

**Not verified.** No model was run, no training was done and nothing was measured, so nothing shows that a model trained on this data uses
`Symbols`, follows the notes or stops repeating itself. The projects are tiny (twelve to fifteen functions; calls are matched by name only,
and the generator never shows a call the index cannot see). The `Find` identifier lookup and the `Symbols` outputs are read with
`strstr` and `sscanf` from the real text, so a change in their format makes the generator drop scenarios (it does not write wrong ones). The
`x-place` scenarios depend on `place_note` and the missing-path search walking the disks of the machine that generates them (names are
random words, but a folder with the same name on a mounted disk would change the note; the generator refuses a note that points elsewhere).
`recall` is only named in the `no-match` output: no conversation calls it. Every file a scenario writes gets its own fixed modification time (`cu_put` stamps an increasing time), so `Grep` and `Glob` list files in the
same order on every replay; if a line still fails the check, `generate.sh` drops it (up to six passes) and lists it in `dropped.txt` (empty in the
last full run). `katana curriculum` removes its partial output file when it aborts with a grammar error (for instance `x-docs` without
`KATANA_VOCAB_MODEL`), so no file cut in the middle of a line is left behind. `x-docs` discards about 190 of 3300 candidate scenarios
("the request would already match a document" / "the engine delivered something where it should be missing"): the situations need a phrase
the engine's document matcher does not match, and the matcher is fuzzy, so the generator draws again; the discards cost generation time only.

## Commands

```
katana curriculum out.jsonl [--n N] [--seed S] [--families a,b=2,...] [--lang pt,en] [--holdout H.jsonl]
                            [--system full|real|blocks|mix:full=F,blocks=B,real=R] [--decl PCT]
katana curriculum stats out.jsonl      # conversations, model turns, characters to learn, 512-token windows (estimate)
katana curriculum check out.jsonl [--all]   # the engine re-does what it can and checks the rest (--all: every reason)
katana curriculum pick pool.jsonl out.jsonl --n N   # chooses by coverage: quotas, tool minimums, one of each tool sequence, unseen cells
katana curriculum variety train.jsonl [validation.jsonl] [-v]   # the measures of this document, with their definitions
katana curriculum matrix out.jsonl     # cells (task x obstacle), stacks, conventions, tools per family
katana curriculum show out.jsonl [index|family|part of the cell] [--sys]   # conversations as plain text
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

Without `--families` only the composed families are generated, in the proportion of the v4 file: `x-rare` 95, `x-mcp` 60,
`x-concept` 58, `x-make` 40, `x-tools` 36, `x-ability` 30, `x-web` 26, `x-fix` 24, `x-config` 24, `x-ps1` 22, `x-clean` 18,
`x-add` 16, `x-rename` 14, `x-investigate` 12, `x-find` 10, `x-explain` 10, `x-skill` 10, `x-run` 8, and since v6 `x-find6` 30, `x-batch` 20,
`x-script` 20 (`x-docs` 30 from v5). The mould families
(`brief-*`, `skill-*`, `mcp-*`, `mixed`) stay in the binary and come out when named in `--families`.

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

## Coder families on the lean tool list (2026-11-23)

No version name: the curriculum is updated in place and regenerated from zero with `tools/curriculum/generate.sh <binary> <folder>`
(pool, holdout, selection by coverage, `check` until stable, `variety`, family counts, SHA256SUMS) and trained with
`tools/curriculum/train.sh <folder>`. Six families in `src/app/scenario_coder.c`, written on the multi-language projects of
`scenario_v7.c`; every call goes through the real dispatcher, every result is the engine's own text, and each scenario works with and
without the context block (the policy reads what the engine returned):

| family | teaches | weight |
|---|---|---|
| `x-nav` | `Symbols` `refs`, `uses`, `tree`, `flow`, `at`; the rule "exact name: Symbols; only words: Find; exact text: Grep" as paired requests; `Find` then `Symbols` | 40 |
| `x-ctx` | the `[katana: context for this request ...]` block placed after the user's text (made by `uioe_prepare` in deterministic mode, `KATANA_UIOE_DET=1`): follow `Next`, complete what the path does not list, the same request when no block appears, a memory line as a dated guess (`- (guess) memory 2026-05-20 ...`), never as a fact; the owner decided the block is taught, so the weight is high and `pick` keeps a quota (see Selection) | 100 |
| `x-ctxlimits` | `Not found / limits`: an existing name beside a missing one (do not search for the missing one again), an ignored folder (`NOT FOUND` plus "N ignored folders could hold it", then `path`) | 20 |
| `x-notes` | the cited folder (engine note or block, `path` from the first call), reading a big file in parts, the repeat note | 15 |
| `x-plan` | `TodoWrite`: `done` sent before any list (the engine's error, then the list), plan with `done=N` in the same turn as the next call | 15 |
| `x-multi` | three requests in one conversation, in English and Portuguese: where is it, who calls it, change it and check the syntax | 25 |

`Glob`, `Rename`, `Replace`, `Patch` and `Task*` are not taught (they are not in the table); a file by name is `Find`, a folder listing is
`Symbols tree` or `Bash`.
