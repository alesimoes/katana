# codebench

A small local coding benchmark for the katana agent. It exists to answer one question with numbers: does a change
(a model, a prompt, a sampling preset, an adapter) make the agent solve more real coding tasks?

- 30 tasks in `tasks/`: the 21 code tasks below plus 5 terminal tasks (`term-*`) and 4 web tasks (`web-*`), described in
  the next two sections.
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

```
python3 tools/codebench/run.py --verify                      # every task must fail before and pass after the reference fix
python3 tools/codebench/run.py --model models/X.gguf --out a.jsonl --label base
python3 tools/codebench/run.py --model models/X.gguf --out b.jsonl --label changed --env KATANA_THINK=1
python3 tools/codebench/run.py compare a.jsonl b.jsonl      # newly solved / lost, exact sign test, mean cost
```

`--tasks a,b` runs a subset, `--timeout` is per task (default 900 s), `--keep DIR` keeps each task's agent log and final
files. Run one model at a time: the GPU and the memory are shared.

`--mode code` runs `katana code` (the coding mode) instead of `katana agent`.

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

`python3 tools/codebench/run.py --verify [--tasks a,b]` runs setup, the hidden check on the untouched starting state (it
must fail), then on a fresh copy after the reference solution (it must pass), and also fails a task if `solution`, `hidden`,
`site`, `setup.py` or `solve.sh` show up in the agent's working folder. It never runs the model.
