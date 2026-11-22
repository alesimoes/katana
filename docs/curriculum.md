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

## Commands

```
katana curriculum out.jsonl [--n N] [--seed S] [--families a,b=2,...] [--lang pt,en] [--holdout H.jsonl]
                            [--system full|real|blocks|mix:full=F,blocks=B,real=R] [--decl PCT]
katana curriculum stats out.jsonl      # conversations, model turns, characters to learn, 512-token windows (estimate)
katana curriculum check out.jsonl [--all]   # the engine re-does what it can and checks the rest (--all: every reason)
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

## Measured on a generated file (seed 7, 2,000 conversations)

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
pass `python3 tools/codebench/run.py --verify`.

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
python3 tools/codebench/run.py --model M.gguf --tasks 'conv-*' --out base.jsonl --label base
python3 tools/codebench/run.py --model M.gguf --tasks 'conv-*' --out spec.jsonl --label spec --env KATANA_SPEC=katana.kspec
python3 tools/codebench/run.py compare base.jsonl spec.jsonl
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
