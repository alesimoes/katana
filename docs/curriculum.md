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
thing. It is written in C (`src/app/curriculum.c`), uses no model, and gives the same output for the same seed.

## Commands

```
katana curriculum out.jsonl [--n N] [--seed S] [--families a,b=2,...] [--lang pt,en] [--holdout H.jsonl] [--system full|blocks]
katana curriculum stats out.jsonl      # conversations, model turns, characters to learn, 512-token windows (estimate)
katana curriculum check out.jsonl      # re-parse, validate every call, check the family invariants
katana import <model.gguf> out.jsonl data.jsonl --system keep
katana specialize <model.gguf> all ... --curriculum out.jsonl     # source added by the specialize owner
```

Each line has the form `{"messages": [...], "source": "curriculum", "family": "<name>"}`.

**`--system full` (the default).** The system message is the agent's own prompt in its short form, without the tools
section, as a specialized model sees it. It is built in-process by `agent_build_sys`. The scenario's environment block
follows, with the skills listed inside it, then `# Project instructions (file .../KATANA.md, ...)` and then the MCP
cards. Import this file with `--system keep`, because `--system file:` replaces the scenario blocks.

**`--system blocks`.** The system message holds only the scenario blocks.

**`train: false` messages.** A few assistant messages carry `"train": false`: a call that breaks a `never:` item before
the engine refuses it, a raw id before the server's error, and a final message sent before the engine's `done:` note.
They stay in the conversation as context and are not trained.

**`--holdout`.** Writes max(50, N/10) conversations built only from reserved slot values: the last quarter of every
content pool, 4 MCP servers and 4 skills. Those values never appear in the training file, so the holdout file measures
generalisation.

The blocks the engine will present are built in one function each, to be switched to the engine's own code when it
lands:

| block | function |
|---|---|
| project block | `cu_sys_project` (and `cu_brief_render`) |
| skills line | `cu_sys_skills` |
| MCP cards | `cu_sys_mcp` (and `cu_card_render`) |
| `Skill` tool result | `cu_skill_result` |
| `never:` refusal | `cu_refusal` |
| `done:` note | `cu_done_note` |

## Families (one short example each)

| family | what the assistant does | example |
|---|---|---|
| `brief-command` | runs exactly the file's command (the labelled one when asked), then reports in words; sometimes a second request follows | "roda os testes de integration" → `Bash cargo test --test integration` |
| `brief-rule` | `layout:`/`rules:` decide where and how; when the user explicitly overrides a guideline rule, it follows the user and says so | new helper → `Write src/helpers/slug.py`; rule "tests next to the code" → two `Write`s and the test run |
| `brief-never-ask` | avoids a `never:` action and does the allowed alternative; reacts to an engine refusal without retrying; asks with `AskUserQuestion` for `ask:` items and acts on the answer | "commita e da push" with `never: push` → `git commit` only |
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

**Quality rules enforced at generation time.** A violation aborts with `ERRO DE GRAMATICA`.

- Every call to a katana tool validates against its `CtTool.props`/`req`.
- Every MCP call validates against the schema of its card.
- A request with 3 or more actions starts with `TodoWrite`, or with `Skill`, which writes the plan.
- Text before a call is one short line at most.
- Every conversation ends with a final assistant message.

**What `check` verifies.** It parses every line again and validates every call. MCP calls are checked against the card
parsed back out of the system message. It also checks these invariants:

- the `brief-command` command is one of the file's;
- no trained `Bash` command matches a `never:` glob;
- in `brief-done`, the done/test command is the last command and comes after the last `Write`/`Edit`;
- in `brief-docs`, the file read is listed in `docs:`;
- in `skill-none`, there is no `Skill` call; the `skill-*` families do call it, with a name from the list;
- in `mcp-chain`, the id comes from an earlier result;
- in `mcp-schema`, `ToolSearch` comes before the call;
- every result matches a call.

A test corrupted on purpose was caught: a changed command, and a number turned into a string.

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
- The blocks follow the spec, not the engine's code, which was not merged yet. Switch the functions listed above when
  it lands, and check that `check` still passes.
- The holdout file exists, but no model has been evaluated on it.
- The data names open-source developer tools (make, cargo, npm, shellcheck...). No product, vendor or inference engine
  is named.
