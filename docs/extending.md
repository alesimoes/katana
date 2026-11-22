# Extending katana: instruction files, skills and MCP servers

Three kinds of documents tell the agent how to work, and all three have a short, machine-checkable format. The engine
uses what it can parse (it enforces rules, checks requirements, writes the task list) instead of leaving everything to
the model's reading. A file that is not in the format still works as plain text.

Nothing here has been tried with a real model: the GPU was busy. Everything below is covered by model-free checks
(`tools/tooltest/extend.sh`) and by the agent run on a tiny random model with a scripted stand-in for the teacher.

## 1. The project file: `KATANA.md` (format "brief")

Plain text, one fact per line, keys at the start of a line, no prose needed.

```
# <name>: <what it is, 12 words at most>
build: <command>
test: <command>
run: <command>
lint: <command>
layout:
  <path>  <what it holds>
rules:
  - <imperative sentence, 100 characters at most>
never:
  - <forbidden action> `<pattern>`
ask:
  - <action that needs the user's yes> `<pattern>`
done: <how to verify that a task is finished>
style:
  - <code convention>
docs:
  <path>  <when to read it>
```

- Every key is optional. A key may carry a label for variants (`test[unit]:`, `test[all]:`). Blocks are indented lines
  under the key. `layout` takes 12 lines at most, `style` 6.
- `docs:` lists pointers the model reads on demand; they cost nothing until needed. There is no include syntax.
- The backtick pattern at the end of a `never:` or `ask:` item is a shell-style glob (`*`, `?`, `[abc]`) over the command
  line of `Bash` (the whole line, or any piece separated by `&&`, `||`, `;`, `|`), or over the path for `Write`/`Edit`
  when it contains a `/` or starts with `*.`. The engine enforces it:
  - `never`: the call is refused with a result that quotes the rule.
  - `ask`: the user is asked through the confirmation path; in a non-interactive run the call is refused with the reason.
  An item without a pattern is guidance only.
- `done:` and `test:` are used by the engine: when a request changed files and is about to end without the `test:` command
  (or the command in backticks in `done:`) having run in that request, one note is attached saying so
  (`brief_end_note` in `src/agent/brief.c`, sent by the agent loop as a user turn). When the file has a `test:` line, the
  note names that command and any file change counts, including a change to a Markdown or `.katana/` file. A `done:`
  command counts as the verification only when it is written in backticks.
- What goes into the system prompt is the file as written. Recommended budget: 1,500 characters. The layered total cap is
  5,000 bytes (`KATANA_INSTR_MAX` changes it); past the cap, the most specific layer keeps its text.
- Ordinary Markdown prose is accepted and passed as text. Nothing is parsed or enforced in it.

### Layers

Read from general to specific: `~/.katana/KATANA.md` (all projects), the project file (`KATANA.md`, or `AGENTS.md`, in the
working folder or above it up to the repository root), and `KATANA.local.md` next to the project file (keep it out of
version control). For single-value keys (`build`, `test`, `run`, `lint`, `done`) the more specific layer wins; block keys
are concatenated, general first; `never:` items can be added by a more specific layer and never removed.

A `KATANA.md` in a subfolder is delivered once per conversation, with the result of the first `Read`, `Edit` or `Write` of
a file in that folder (not in the system prompt, so the prompt cache stays valid).

### Commands

- `katana init [dir] [--force]` and `/init` in the agent write a first file from what can be detected, without a model: build
  system and commands (Makefile targets, CMake, `package.json` scripts, `pyproject.toml`/`setup.py`, `Cargo.toml`,
  `go.mod`, `build.sh`/`test.sh`/`run.sh`), top-level layout with a guess from folder names and file types, the languages by
  file count (when there is no README), the README's first sentence, and `docs:` pointers. `/init` then asks the model to
  improve the file by reading the project. An existing file is never overwritten without `--force`.
- `katana brief check [file]` lints: unknown keys, lines over the limits, prose outside keys, hedging words ("try to",
  "maybe", "if possible", "should probably", "se possivel", "talvez", ...), duplicate items, a pattern that does not compile,
  a `build:`/`test:` program that is not on PATH, the size against the budget. Exit code 1 on errors, warnings are listed.
- `katana brief show [file]` prints what the engine parsed and what it enforces; without a file, the merged layers.

### Worked example

```
# pixelsort: command line tool that sorts image pixels by brightness
build: cc -O2 -o pixelsort src/*.c
test: sh tests/run.sh
layout:
  src/  C source, one file per sorting mode
  tests/  shell tests with sample images
rules:
  - Keep every function under 60 lines.
  - Write a test for each new sorting mode.
never:
  - overwrite the sample images `tests/img/*`
  - force push `git push*--force*`
ask:
  - delete a folder `rm -r*`
done: `sh tests/run.sh` passes
docs:
  docs/modes.md  read before adding a sorting mode
```

Enforced: both `never` items (a `Write` to `tests/img/a.png` or a `git push --force` is refused, quoting the rule), the
`ask` item, and the end-of-request note about `sh tests/run.sh`. Guidance only: the `rules`, `layout`, `docs` lines and the
header (the model reads them).

## 2. A skill: `SKILL.md` (format "ability")

```
---
name: <identifier>
when: <one line: the situation in which to use it>
needs: <programs, tools or inputs it requires, comma separated>
args: <named inputs, comma separated>
---
steps:
1. <imperative action> => <check that shows it worked>
2. <imperative action> => <check>
done: <observable end state>
report: <what to tell the user at the end>
fail: <what to do when a step's check fails>
```

- `when` replaces `description` (`description` is accepted as an alias). `=> check` is optional per step.
- The engine uses it when the skill loads (`Skill` tool, `/skillname`):
  1. `needs` is checked first: a program (on PATH), a tool of the tool table, a connected MCP server, or an input named in
     `args` (passed by the model as `args` of the form `name=value other=value`). Anything missing is reported at the top
     (`MISSING: ...`) so the model tells the user instead of discovering it by failing. No plan is written then.
  2. If no task list is open in this request, the list is written from `steps` (through the same code as `TodoWrite`, so the
     desktop card and the plan reminders work) and the result says "Plan written from the skill: do step 1."
  3. Each step is shown with its check; `done`, `report` and `fail` come at the end. `$name` in a step, `done`, `report` or
     `fail` is replaced by the value of that argument.
  4. Extra files in the skill folder (scripts, reference documents) are listed with full paths.
- A skill written as free Markdown still loads as text; no plan is written.
- Lines starting with `#` inside the body are notes for the author and are not shown to the model.
- Lookup order: `<cwd>/.katana/skills`, `<cwd>/.config/katana/skills`, `~/.katana/skills`, `~/.config/katana/skills`.
- The environment block of the system prompt has one line, `- Skills (load one with the Skill tool): name: when; ...`
  (at most 10 skills and 600 characters, `when` cut at 60, project skills first, then global; with more, the number of the
  others and the hint that `Skill` lists them). No skills, no line.

Commands: `katana skill new <name> [--global] [--description TEXT]`, `list`, `show <name>`, `remove <name>`, `path`,
`check [name]`. `check` reports a missing `when` (or the template text left in it), a missing or empty `steps`, a step
that is not imperative or has over 120 characters, more than 9 steps, `needs` naming something that does not exist, and no
`done`. In the agent: `/skill new <name> <what it should do>` creates the file and asks the model to write it properly,
`/skill list|show|remove|check`, and `/<skillname> arguments` runs the skill (loads it, then sends its text with your words
as the message). Built-in commands win over a skill with the same name.

### Worked example

```
---
name: release
when: Cut a release of a named version
needs: git, version
args: version
---
steps:
1. Check the working tree is clean => git status prints nothing
2. Run the tests => the command exits with 0
3. Tag the release as v$version => git tag lists v$version
done: the tag v$version exists on the current commit
report: the version, the tag and the test result
fail: stop at the failing step and say which check failed
```

Run it with `/release version=1.4`: `git` is checked on PATH, `version` against the arguments, the three steps become the task
list, and `$version` becomes `1.4`.

## 3. An MCP server: the card (format "card")

What the model sees of a connected server is a compact card generated from the server's own declarations, not its JSON
schemas:

```
mcp <server>: <what it is for>
  <server>__<tool>(<arg>, <arg>:number, <arg>?:string[]) - <description cut at 60 characters>
  ...
first: <calls to make before the others>
avoid: <what not to do>
```

- Signature: required args first (in the order the server lists them), a type suffix only when it is not a string
  (`:number`, `:integer`, `:boolean`, `:object`, `:string[]`, `:number[]`, ...), `?` marks an optional arg, an enum is written
  `arg:(a|b|c)`, at most 8 args are shown, then `...`. The full JSON schema stays available through `ToolSearch` (also by
  the `mcp__server__tool` spelling).
- `<what it is for>`, `first:` and `avoid:` come from an optional usage file written by the owner, `mcp/<server>.md` beside
  the config file that declares the server (`~/.katana/mcp/` or `<project>/.katana/mcp/`), with exactly the keys `what:`,
  `first:`, `order:`, `avoid:` (one line each; `order:` is printed after `first:`). Without the file, `what` is the server's
  own `instructions` field from the handshake (first 120 characters), else the server name alone.
- The cards go into the system prompt (section `# MCP servers`) instead of JSON declarations, capped at 2,500 characters in
  total; beyond that each server keeps its first line and the tool names only.

### Configuration

`~/.katana/mcp.json` (or `KATANA_MCP_CONFIG`) and the project's `.katana/mcp.json` (working folder or above, up to the
repository root), both of the form
`{"mcpServers": {"name": {"command": ..., "args": [...], "env": {...}} | {"url": ..., "headers": {...}}}}`; an entry may
have `"prefix": "mcp__"`, and `KATANA_MCP_PREFIX` sets a default for all servers. Project entries win on a name clash and
`katana mcp list` says which file each server came from. A project file can start arbitrary programs, so the first time
(and whenever the file changes) the user is asked once; the answer is remembered per path and content hash in
`~/.katana/mcp-trust`. Without a terminal or the desktop, untrusted project servers are skipped and the message says so.
`KATANA_MCP_TRUST=1` trusts without asking, `0` never trusts. `katana mcp add --project` trusts the file it writes.

Naming: katana declares `server__tool`; a call written as `mcp__server__tool` is accepted too (as other applications
spell it); with a prefix configured, the declared name carries it.

A server of your own file with `"channel": true` may push messages into the console (`docs/channels.md`).

### Commands

`katana mcp add <name> -- <command> [args...]`, `katana mcp add <name> --url <url> [--header 'K: V']...`, `--env K=V`
(repeatable), `--prefix P`, `--project`; `katana mcp remove <name> [--project]`; `katana mcp list`; `katana mcp test <name>`
(connect, tools, resources, prompts, timing, errors in plain words, exit code); `katana mcp card <name>` (the card exactly as
the model sees it, with its size). `add` creates the usage file with empty keys and says so. In the agent: `/mcp` (state of each
server: connected, tools, prompts, last error), `/mcp reconnect <name>`, `/mcp card <name>`, and `/<server>:<prompt> args`
which runs a prompt of the server as your own message (prompts are for the user; the model does not see the list).

Protocol: `tools/list` pagination and `notifications/tools/list_changed` (the list is reloaded and the result says so),
`structuredContent` (compact JSON after the text unless the text already has it), `isError` results, image, audio, resource
and resource-link blocks described in one line, `prompts/list` and `prompts/get`, a stdio server that died is restarted once
on the next call (an HTTP session that expired too), a call that gets no answer says how long it waited
(`KATANA_MCP_TIMEOUT`, seconds). Not handled: sampling, elicitation, roots and progress requests from the server (it gets
"method not found"), resource subscriptions, OAuth.

More protocol coverage (variables in the configuration, roots, annotations, elicitation, SSE, OAuth, `katana mcp serve`)
is in `docs/mcp.md`.

### Worked example

```
katana mcp add tickets --env TOKEN=abc -- node /opt/tickets/server.js
```
then edit `~/.katana/mcp/tickets.md`:
```
what: ticket tracker of the support team
first: call tickets__whoami to confirm the login
order: search before create, to avoid duplicates
avoid: closing tickets without the user's yes
```
and `katana mcp card tickets` prints
```
mcp tickets: ticket tracker of the support team
  tickets__whoami() - Show the logged-in user
  tickets__search(query, status?:(open|closed), limit?:integer) - Find tickets by text
  tickets__create(title, body, tags?:string[]) - Create a ticket
first: call tickets__whoami to confirm the login
order: search before create, to avoid duplicates
avoid: closing tickets without the user's yes
[N characters: the real count is printed]
```

## Measured sizes (system prompt, `katana agentsys`, empty folder, 2026-11-21)

| case | bytes |
|---|---|
| before this work | 8,603 |
| after, no skills and no MCP servers | 8,603 |
| two skills | 8,698 |
| two skills and the test server (7 tools, card) | 9,254 |

The card of the 7-tool test server is 473 characters; the JSON declarations the old code wrote for the same tools were about
1,560 (estimated from the old format). The fixed text this work adds to the prompt is the skills line prefix (about 45
characters) and the card section title (about 80), both only when there is something to list.

## Not wired, not tried

- `brief_end_note()` (the end-of-request note about `test:`/`done:`) is wired: the agent loop calls it where a request
  ends without a tool call, before the unfinished-todo reminder (`src/agent/loop.c`). The `never`/`ask` enforcement and
  the skill plan are live in the loop too (they sit in the tool dispatcher).
- Nothing was tried with a real model (Qwen3.6, Gemma 4): the format of the card, the skills line and the new commands were
  only exercised without a model. Whether the models call skills from the one-line list, and whether the call-format
  constraint copes with MCP tools that are no longer declared in `<tools>`, is not measured.
- The Windows build was not compiled with the new files (they use POSIX calls: `realpath`, `dup2`, `chmod`).
