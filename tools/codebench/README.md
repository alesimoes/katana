# codebench

A small local coding benchmark for the katana agent. It exists to answer one question with numbers: does a change
(a model, a prompt, a sampling preset, an adapter) make the agent solve more real coding tasks?

- 63 tasks in `tasks/`: the 21 code tasks below, 5 terminal tasks (`term-*`), 4 web tasks (`web-*`), 9 news tasks (`news-*`),
  12 code-writing tasks (`write-*`, `prereq-*`, `extend-*`) and 12 convention tasks (`conv-*`), described in the sections below.
- The original 21 tasks: 18 are Python (standard library only): 12 bug fixes (one spans two modules), 3 features written from
  a stub (a calculator, a persistent to-do list, a duration parser) and 3 larger jobs: `mdhtml` (a Markdown subset to
  HTML converter, about 80 lines), `kvstore` (a package written from nothing: a log-structured store plus a command
  line, two files) and `ledger` (a feature that touches three modules of an existing package and adds a fourth). 3 are terminal and navigation jobs: `sh-backup` (fix a bash script that breaks on names with
  spaces), `find-port` (find one configuration file among 48 and report from it) and `log-top` (rank addresses in a
  4,400-line log).
- Each task has `instruction.md` (the user's request), `repo/` (the starting project), `hidden/` (unit tests the agent
  never sees) and `solution/` (a reference fix used only by `--verify`).
- The agent starts in a temporary copy of `repo/`, gets the instruction as its first message, and works with its normal
  tools. Then `hidden/` is copied in and run with `python3 -m unittest`. Pass or fail, wall time, tool calls and
  generated tokens are appended to a JSONL file.

The runner is part of the binary: `katana codebench` (C, `src/app/codebench.c`). The tasks are test material in whatever
language they are about (here Python, shell and C); only the setup helpers and the hidden tests need `python3`
(`--python` changes the interpreter). POSIX only for now.

```
katana codebench verify                      # every task must fail before and pass after the reference fix
katana codebench run --model models/X.gguf --out a.jsonl --label base
katana codebench run --model models/X.gguf --out b.jsonl --label changed --env KATANA_THINK=1
katana codebench compare a.jsonl b.jsonl     # newly solved / lost, exact sign test, mean cost, agent-log measures
katana codebench list                        # the tasks
```

`--tasks a,b` runs a subset (names or globs: `--tasks 'write-*,prereq-*'`), `--timeout` is per task (default 900 s), `--keep DIR` keeps each task's agent log and final
files. Run one model at a time: the GPU and the memory are shared.


Other options of `run`: `--repeat N`, `--label L`, `--katana BIN` (the agent binary; default is the running binary, and any
program that takes `agent MODEL` works as a stand-in), `--env K=V` (repeatable; given to the agent and to the hidden tests),
`--dir PASTA` (the codebench folder; default `tools/codebench` or next to the binary), `--teacher URL --teacher-model M
--distill-out F [--teacher-key-env VAR] [--teacher-format qwen|json|gemma4|glm]` (a teacher model solves each task with
katana's tools through `katana distill`, and the hidden tests set `passed`). An interrupted run continues where it stopped
(the same file, the pairs task and attempt already in it are skipped).

Each task runs in a fresh folder under `$TMPDIR` (never in the repository), the agent as a child process in its own process
group with its own `KATANA_LOG` file; at the time limit the whole group is killed, and when the agent ends so is whatever it
left running. A task whose setup fails (a news task without internet) is written as `"skipped": true` and does not count.

Result line (JSONL), in this order: `label`, `task`, `passed`, `seconds`, `tool_calls` (counts the call line and the result
line of each call, as the old script did), `gen_tokens`, `timed_out`, then `attempt` (with `--repeat`), `session` and
`instruction` (with `--sessions`), then the measures read from the run's own agent log and stderr: `calls`, `failed_calls`,
`refused_calls` (calls started minus calls logged; the engine's refusals, such as a third identical failing call, never
reach the log), `repeated_calls` (same tool and arguments among the previous 20 calls), `rounds`, `last_text` (the run ended
on text, not on a call) and `tokens_read` / `tokens_written` (the "entrada" and "resposta" token counts the agent prints each
round). Files from before 2026-11-21 lack the last eight fields; `compare` averages them only over the runs that have them.

`--sessions DIR` saves one agent session per run (`DIR/<task>.<attempt>.ksess`) and `--repeat N` runs every task N
times; together they produce labelled training data (see `docs/training-data.md` and `tools/dataset/ksess2jsonl.py`).
With `--repeat`, `compare` keeps only the last attempt of each task.

Limits to keep in mind: 21 tasks is a small sample, so only large differences mean something (the sign test prints the
p-value); the tasks are small and in one language; the agent can read the visible code and write its own tests, but
never sees `hidden/`.

## Terminal tasks (`term-*`)

These can only be solved by really using the shell; the answer goes to a file that the hidden test checks.

- `term-findproc`: setup starts three processes; find which one really listens on a given TCP port (its command line
  does not show the port, a decoy mentions it without listening) and write its PID and working directory.
- `term-gitbug`: setup builds a deterministic 20-commit git repository; find the commit after which `check.py` fails
  (its message is innocent) and write its hash, leaving history and tree untouched.
- `term-vault`: `vault.py` is an interactive program that asks three questions on stdin; the answers come from files and
  the access code it prints depends on a salt that changes on every run (`VAULT_SALT`, given to the agent's environment).
- `term-fixbuild`: a C project whose `make` fails three times in a row (wrong include path, `-Werror` on an unused
  variable, a missing object at link time).
- `term-logs-gz`: logs split in a plain file and gzip rotations; distinct request ids and busiest hour of ERROR lines.

## Web tasks (`web-*`)

They are offline and deterministic: the task's `setup.py` copies `site/` (the fixture pages, never visible to the agent) to
a temporary folder and serves it with `python3 -m http.server` on 127.0.0.1 on a free port; the URL replaces `{url}` in
the instruction. The server is stopped when the task ends.

- `web-follow`: index page, then an unsorted release table, then the detail page of the newest `lts` release (checksum).
- `web-json`: a paginated JSON API (`next` links); sum a field with two filters over all pages.
- `web-longdoc`: one long reference page where the same option name appears in several sections with different defaults.
- `web-report`: read the docs page, write `report.py BASE_URL` that downloads a CSV and prints a result; the hidden test
  runs it against a different dataset on another port (`hidden/alt_site/`), so hardcoding fails.

The agent's `web_fetch` tool (`src/agent/web.c`, `webnav.c`) only checks that the URL is http or https and runs the system
`curl`; it has no block list for loopback or private addresses, so 127.0.0.1 is reachable (unless `http_proxy` is set in the
environment without `NO_PROXY=127.0.0.1`, which would send curl through the proxy).

## Code-writing tasks (`write-*`, `prereq-*`, `extend-*`)

Twelve tasks that can only be solved by creating source files. Hidden tests check behaviour (exit codes, UTF-8, empty
input, bad arguments), never the file names the instruction leaves open; a test that must run the program finds it
(the only `.py` or `.sh` in the folder, or the executable that `make` produces). Eight instructions are in informal
Portuguese, four in English. Python standard library, POSIX shell and `gcc` only, offline.

- `write-*`: an empty project plus an instruction. `write-todo-cli` (CLI with subcommands and a JSON file),
  `write-wordstats` (a package of three modules that import each other, plus `python3 -m`), `write-c-stats` (a C program
  and a Makefile; the test builds it and checks warnings with `-Wall -Wextra`), `write-sh-organize` (a POSIX `sh` script
  that sorts files by extension without overwriting), `write-http-notes` (a JSON HTTP server started on a free port),
  `write-miniconf` (parser and formatter of a small config format, errors with line numbers, round trip).
- `prereq-*`: the code needs something that is not on the machine, like the real failure of a model that got stuck on a
  missing SDK. `prereq-quillpress` (a made-up command; the test runs the script without it and with a stub on `PATH`),
  `prereq-datavault` (a credentials file in `$HOME` and a made-up upload API; the test provides a temporary `HOME` and a
  local server), `prereq-pingd` (a TCP metrics service on a closed port; the test provides a tiny local server). The
  reference solution writes the code anyway, fails cleanly (message, exit code, no traceback) and ships a README.
- `extend-*`: a project of 300 to 400 lines over several files (with its own tests in `tests/`); the instruction needs
  a new module and changes in existing files. `extend-library` (late fees), `extend-agenda` (recurring events, month-end
  clamping), `extend-salesrep` (a Markdown table format plus an output file option). The hidden test also runs the
  project's own tests, so breaking existing behaviour fails the task.

All twelve pass `--verify`; no model has been run on them yet.

## News tasks (`news-*`)

Nine questions about things that change over time, asked in Portuguese, which the model cannot answer from memory
and has to look up on the real internet with `web_search` and `web_fetch`. They need a working connection. The
right answer is not stored in the task: `setup.py` fetches it from a public source just before the agent runs and
hands it to the hidden test (never to the agent), so the tasks do not go stale.

| task | question | where the right answer comes from |
|---|---|---|
| `news-presidente-br`, `news-presidente-eua`, `news-premie-uk`, `news-papa` | who holds the office today | Wikidata (current holder, with aliases) |
| `news-dolar` | US dollar in reais today (3% tolerance) | a public exchange-rate API |
| `news-selic` | current Selic target rate | Banco Central do Brasil API |
| `news-python`, `news-kernel` | latest stable version (the series must match) | endoflife.date, kernel.org |
| `news-manchete` | a news story from today or yesterday, title and URL | the test opens the URL: the title must be on the page and the page must carry a date from the last three days |

Limits: the sources can lag behind an event by hours or days (a new office holder, for instance), `news-manchete`
checks that the story exists and is recent, not that it is important, and a task whose source cannot be reached
is recorded as `skipped` instead of stopping the run.

## Per-task hooks (setup, solution scripts)

All optional, so the original tasks work unchanged.

- `tasks/<name>/setup.py` defines `setup(work)` and returns a dict with any of: `vars` (text substituted for `{key}` in
  `instruction.md`; also exported as `CB_<KEY>` to the hidden tests and the reference solution, never to the agent),
  `agent_env` (extra environment variables, given to the agent too) and `stop` (cleanup function, called when the task
  ends, e.g. stops the local server). `bench_lib.py` has `free_port()` and `start_site(dir)` for this.
- `solution/solve.sh`, when present, is run in the working folder (with the same environment) instead of copying
  `solution/` over it. Only `repo/` is copied for the agent: `hidden/`, `solution/`, `site/` and `setup.py` stay out.
- `hidden/` may carry extra data (for example `alt_site/`), copied only after the agent has finished.

## Validating tasks without the model

`katana codebench verify [--tasks a,b]` runs setup, the hidden check on the untouched starting state (it
must fail), then on a fresh copy after the reference solution (it must pass), and also fails a task if `solution`, `hidden`,
`site`, `setup.py` or `solve.sh` show up in the agent's working folder. It never runs the model. A task whose setup cannot run here (no internet for the news tasks) is shown as `skipped`.

## Convention tasks (`conv-*`)

Twelve tasks that measure whether the agent follows katana's conventions, not whether it can code. The instruction is a
short informal request that never restates the convention; the agent has to get it from `KATANA.md`, a skill in
`.katana/skills/` or an MCP server. Passing is checked only through side effects (marker files, step logs, the MCP
call log), never by reading what the model says. State files live in `.katana/state/` inside the working folder; MCP
call logs live in a temporary folder outside it (`CB_MCPLOG` for the hidden test). All names are invented.

| task | convention | what the hidden test requires |
|---|---|---|
| `conv-brief-test` | `test:` is `sh tools/check.sh --suite fast` | the bug is fixed and the marker written only by that command exists, is `fast`, and is not older than `cart.py`. `make test` leaves a different marker that is tolerated but does not count |
| `conv-brief-layout` | `layout:` and a rule: new handlers go in `src/shipdesk/handlers/` (a `legacy/` folder with look-alike modules is the trap) | `shipdesk.handlers.cancel_label` imports and works, and no other `cancel_label` exists anywhere |
| `conv-brief-never` | `never:` item with the pattern `*tools/regen.sh*` | the fixture was hand edited and the generator (which would work and leaves a tripwire) never ran |
| `conv-brief-docs` | `docs:` points to `docs/units.md` (cents, basis points, half-up rounding) | `add_tax` gives the exact values that only follow from those facts |
| `conv-brief-done` | `done:` is `sh tools/verify.sh`, which stamps a hash of `src/` | the stamp equals the hash of the final `src/` (the command ran after the last edit) |
| `conv-skill-order` | skill `cut-release`, four script steps | the log shows check, bump, notes, seal in that order, plus the done state |
| `conv-skill-args` | skill `ship-build` with `args: version, channel` | the log lines carry the values from the request (3.14.2, nightly) |
| `conv-skill-needs` | skill `build-docs` needs `inkpress`, which does not exist; its `fail:` says what to do | `.katana/state/missing.txt` names it, the two independent steps ran, no PDF and no fake `inkpress` |
| `conv-skill-choose` | two similar skills (`backup-db`, `backup-files`); only one `when:` fits | only the files skill's steps are in the log |
| `conv-mcp-call` | project `.katana/mcp.json`, label printer server | one `print_label` call with a JSON integer `copies`, enum `medium`, exact keys |
| `conv-mcp-chain` | ticket tracker; ids are random per run | `list_tickets` came first and `close_ticket` used the id of the right ticket |
| `conv-mcp-schema` | irrigation controller, 9 arguments with a nested `valve` object | the exact nested structure, `dry_run` true, correct types |

Each MCP task ships its own `server.py` next to `setup.py` (not in `repo/`); `setup.py` writes `.katana/mcp.json` in the
working folder pointing to it, with a `--log` argument; the server appends every `tools/call` (name and arguments) to
that file. The reference solution (`solve.sh`) is a small inline stdio client that reads `.katana/mcp.json`. These tasks
depend on the engine showing `KATANA.md` and the skill list to the model, enforcing `never:` patterns on Bash, loading
skills with the Skill tool, and exposing project MCP tools as `server__tool`.
