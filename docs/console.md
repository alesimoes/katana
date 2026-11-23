# The katana command: console in your project, headless mode, install, adopt

Code: `src/app/launch.c`. Tests: `tools/tooltest/launch.sh` (and `tools/tooltest/console_pty.py`, driven by it).

## 1. Install the command

```sh
./katana install                 # symlink ~/.local/bin/katana -> this binary
./katana install --dir ~/bin     # another folder
./katana install --force         # replace a different file or link that is already there
katana uninstall [--dir DIR]     # removes the symlink (only if it points at this binary; --force otherwise)
```

`install` checks that the folder is on `PATH` (and prints the `export PATH=...` line to add to `~/.bashrc` or `~/.zshrc` when it is not),
and checks that the models folder is found from any working directory.

Models folder, in order: `KATANA_MODELS`; `models=` in `./.katana/katana.cfg` or `~/.katana/katana.cfg`; `./models` when it exists;
the `models` folder beside the **real** path of the binary (the symlink is resolved, so running `katana` from any folder through the
link finds the models that live next to the repository's binary); `~/.katana/models`. When the models live beside the binary,
`katana install` writes `models=<path>` to `~/.katana/katana.cfg` (other keys of that file, such as the GPU tuning, are kept; `katana tune`
keeps them too).

Other files the engine reads are independent of the working directory: shaders are compiled into the binary; shard, heat and profile
files sit next to the model (`<model>.ktn`, `.ktn.heat`, `.ktn.prof`); the GPU tuning file `katana.cfg` is looked up beside the real binary
first, then in the working directory, then in `~/.katana`.

## 2. Start in a project

```sh
cd ~/my-project
katana                      # agent console in this folder
katana "fix the failing test"   # same, the first message is sent at once
katana --model qwen -c      # pick a model by name, continue the last session of this folder
```

`katana [options] ["first message"]` is used when the first argument is not a subcommand and not a `.gguf` path
(subcommands: `katana --help` lists the options; every older subcommand still works, `katana agent <model.gguf>` included).

The console runs in the current folder: the project's `KATANA.md` (or `AGENTS.md`), `.katana/skills`, and the MCP servers of
`~/.katana/mcp.json` are used as the agent already does. `--cwd DIR` starts in another folder.

### Model selection

First match wins, and one line says which model was chosen and why (`katana: model <path> (<reason>)`):

1. `--model NAME|PATH`
2. `KATANA_MODEL` (same forms)
3. `model=` in `./.katana/katana.cfg`, then in `~/.katana/katana.cfg`
4. the model the desktop app used last (`ultimoModelo=` in `~/.katana/desktop/ui.ini`), if it is still installed
5. the catalog's recommended model, if installed
6. the largest installed model

A NAME is matched against the `.gguf` files of the models folder: exact file name (with or without `.gguf`), then a case-insensitive
substring. More than one match: the candidates are listed and the exit code is 2. A stale `model=` or `ultimoModelo=` is skipped with a
warning; a wrong `--model` or `KATANA_MODEL` is an error. With no model installed it prints `katana catalog` / `katana pull <id>` and
exits 2.

### Sessions per folder

Sessions of the console are stored in `~/.katana/sessions/<hash of the folder path>/` as `<id>.ksess` (id = date, time and a suffix),
with an `index` file (one line per session: `id TAB epoch TAB turns TAB first message`) and the `history` file of the line editor.
The desktop app's sessions stay where they are (`~/.katana/desktop/sessoes`).

| option | meaning |
|---|---|
| `-c`, `--continue` | resume the most recent session started in this folder, or in the root of its git repository |
| `-r`, `--resume [ID]` | without ID: list this folder's sessions (number, id, date, turns, first message) and ask which one to resume (when not on a terminal it only prints the list); with an ID (or the start of one): resume it |
| `--session-id ID` | use this id for the session file; if it exists, it is resumed (letters, digits, `-`, `_`) |
| `--no-session` | do not save |

A session written by another model is not resumed (the console says so and starts a new one) unless `KATANA_RETOMAR_OUTRO=1` is set;
`/model` sets it for its own reload, so the conversation survives a model switch.

### What the console shows (terminal only)

When stdout and stderr are terminals the console is "rich". With `NO_COLOR` set the same structure appears without colours or spinner
(ASCII marks); when output is a pipe or file, or `KATANA_PLAIN=1`, nothing changes from the older plain output.

* A header: folder, model and why, context size, MCP servers (how many are up, from which files), skills (count per folder), instruction
  files loaded (with sizes), session file.
* Each tool call is one line: name, its `description` argument when present (otherwise the main argument), a spinner and the elapsed time
  while it runs; at the end a check or cross, the duration, the raw command dimmed, the first lines of the output dimmed and the rest
  folded as `... +N lines`. Notes the engine adds for the model (`[katana: ...]`, `[plan: ...]`) are not shown.
* The task list as a block when it changes, the step in progress marked.
* Answers are rendered with a little Markdown: bold, inline code, code blocks, lists, headings; the model's raw tool-call text is hidden.
* After each request a status line: time, tokens in and out, tokens per second, context used.
* Line editing without any library: arrows and Home/End, Up/Down for the history of this folder (kept in `~/.katana/sessions/<hash>/history`),
  Ctrl+A/E/B/F/K/U/W, Ctrl+L, Alt+B/F; a backslash at the end of a line or Alt+Enter continues on a new line; **pasted text is one
  message** (the terminal's bracketed paste marks are used; newlines inside show as a return symbol). Piped input is read exactly as
  before (`KATANA_PLAIN_INPUT=1` forces that on a terminal). While you type, scheduled tasks and messages from other sessions wait for the
  Enter.

### Help by word

`katana help <word...>` prints only the lines of `katana --help`, `katana help` and the console's `/help` that contain a
word, grouped by where they come from; with no match it says so and exits 1. The agent uses it to answer questions
about katana itself without putting the whole help in the context (the console commands had no other source).

### Slash commands

| command | |
|---|---|
| `/new`, `/clear` | clear the conversation |
| `/compact` | summarize and shrink the old context |
| `/system <text>` | set extra instructions (restarts the conversation) |
| `/save <file>` | write the conversation to a text file |
| `/plan`, `/plan off`, `/go` | plan mode (read-only until `/go`) |
| `/undo` | put back the files changed by the last request that changed any |
| `/cd <folder>` | change the working folder |
| `/session <file>`, `/resume <file>` | save to / load a session file |
| `/teacher [on\|off\|status\|<model>]` | let a larger model answer in place of the local one, through the teacher plugin (`docs/teacher-protocol.md`) |
| `/model [name]` | list the installed models; with a name, save the session and reload the engine with that model (same process, same terminal; on Windows it asks for a restart) |
| `/status` | model, context use, session file, MCP servers, skills, instruction files |
| `/help` | list them |
| `/exit`, Ctrl+D | leave. Ctrl+C interrupts the answer |

## 3. Headless mode: one request in, one answer out

```sh
katana -p "summarize the failing tests" --output-format json --mcp-config servers.json --strict-mcp-config --allowed-tools 'Read,Grep,mcp__docs__*' --max-turns 12
echo "what changed?" | katana -p            # the request from stdin
```

The request runs to the end with nobody to ask: a tool that needs the user (`AskUserQuestion`) answers as it does for piped input,
"nobody can answer; decide by yourself". Exit code **0** with the final answer on stdout, **1** with the reason on stderr (and, with
`json` or `stream-json`, the result object still goes to stdout with `is_error: true`), **2** for bad options, an unknown model, an unreadable
`--mcp-config`. The request is limited to 16000 bytes. Everything the engine says while it works (loading, tool lines, MCP start) goes to
stderr; `--quiet` silences it (the reason of a failure is still printed). The model choice follows section 2; `KATANA_CPU=1`, `KATANA_CTX`
and the other variables of the engine work as always.

| option | meaning |
|---|---|
| `-p`, `--print [TEXT]` | headless; without TEXT the request is read from stdin |
| `--output-format text\|json\|stream-json` | default `text`: only the final answer |
| `--mcp-config FILE\|JSON` | MCP servers for this run: a path, or an inline JSON object with `mcpServers` |
| `--strict-mcp-config` | use **only** the servers of `--mcp-config` (none if absent): the user's and the project's files are ignored (`KATANA_MCP_STRICT=1`) |
| `--channel NAME` | this MCP server may push messages into the console and answer confirmations (`docs/channels.md`) |
| `--allowed-tools 'a,b'` | only these tools; shell-style globs (`*`, `?`) over tool names; MCP tools match under both spellings `server__tool` and `mcp__server__tool`; `mcp__server` allows a whole server; `Bash(git:*)` counts as `Bash` |
| `--disallowed-tools 'a,b'` | never these (wins over the allow-list) |
| `--max-turns N` | at most N model turns (rounds) for the request; no extension for progress |
| `--system-prompt-file F` | replace the rules part of the system prompt with the file (the tool declarations and the environment block stay) |
| `--append-system-prompt TEXT` | add text at the end of the system prompt |
| `--cwd DIR` | work in DIR |
| `--session-id ID` / `--no-session` | save under this id (resumed if it exists) / do not save |
| `--model NAME\|PATH`, `--quiet` | as above |

A tool outside the allow-list is **not declared** to the model (core, deferred-by-name and MCP tools alike) and, if called anyway, the call is
refused with a result starting `error: the tool X is not allowed in this run`. The check lives in the tool dispatcher, so it also applies
to `katana tool` with `KATANA_ALLOWED_TOOLS` / `KATANA_DISALLOWED_TOOLS` set (those are what the options set).

### Output shapes (the contract)

Names and values below are English and stable; fields may be added, never renamed.

**`--output-format json`**: exactly one JSON object on stdout, one line, after the work ends:

```json
{"type":"result","subtype":"success","result":"Done: the file is written.","is_error":false,"num_turns":4,"duration_ms":7252,
 "session_id":"20261121-194416-185","usage":{"prompt_tokens":8707,"completion_tokens":678},
 "tool_calls":[{"name":"Write","arguments":{"file_path":"a.txt","content":"x\n"},"is_error":false}]}
```

* `result`: the final answer (the last model message, without reasoning or tool-call markup); `""` when there is none.
* `is_error`: `true` when the run did not end with a final answer (exit code 1).
* `subtype`: `"success"`, `"error_max_turns"` (the turn limit ended it before an answer), or `"error"` (anything else).
* `num_turns`: model turns taken (each answer, including the ones that call tools). `duration_ms`: wall time of the request.
* `session_id`: id of the saved session (`<id>.ksess` in `~/.katana/sessions/<hash of the folder>/`), `null` with `--no-session`.
* `usage`: `prompt_tokens` (tokens the model read, summed over the turns; with the cache it is what was fed this run) and `completion_tokens`.
* `tool_calls`: every call made, in order: `name`, `arguments` (the object the model sent), `is_error` (the result began with `error:` or the command's
  exit code was not zero; a call refused by the allow-list counts as an error).

**`--output-format stream-json`**: one JSON object per line, as things happen, flushed at each line:

```
{"type":"text","text":"I will write the file."}                                  one per model message that has text, when the message is complete
{"type":"tool_call","name":"Write","arguments":{"file_path":"a.txt","content":"x\n"}}
{"type":"tool_result","name":"Write","is_error":false,"summary":"File created successfully at ..."}   summary: the first 240 characters, one line
{"type":"plan","steps":[{"content":"step a","status":"in_progress"},{"content":"step b","status":"pending"}]}   when the task list changes (status: pending, in_progress, completed)
{"type":"result", ...}                                                           last line: the same fields as the `json` object
```

For each tool call the order is `tool_call`, then `tool_result`, then `plan` if the call changed the task list. The text of a message comes before
its tool calls. The last line is always the `result` object, also on failure.

**`text`**: the final answer and a newline on stdout; nothing else (on failure: nothing, exit 1).

Running the engine as the "assistant" of another application: the application starts `katana -p ... --output-format stream-json` in the
project folder, reads lines until `type == "result"`, and treats exit code 1 or `is_error: true` as a failed run.

## 4. Bring an existing project over: `katana adopt`

```sh
katana adopt [dir] [--instructions FILE] [--mcp FILE] [--skills DIR] [--dry-run] [--force]
```

For a project that already has an agent setup made for another tool; every path comes from the options (nothing is looked up by name).

* `--instructions FILE`: copied as is to `<dir>/KATANA.md` (the console reads plain text; `/init` inside the agent can rewrite it in katana's compact format).
* `--mcp FILE`: a file with the `{"mcpServers": {...}}` shape; its servers are merged into `<dir>/.katana/mcp.json` (servers already there are kept; `--force` replaces the ones with the same name).
* `--skills DIR`: every subfolder of DIR with a `SKILL.md` (or DIR itself when it has one) is copied to `<dir>/.katana/skills/<name>/`, with its other files.
* Nothing is overwritten without `--force`; `--dry-run` only prints. At the end it prints what was written and what to check: the commands and keys of the MCP servers,
  skills or instructions that name tools katana does not have.

## 5. Not tried with a real model

All the headless and console tests use a 0.1M-parameter random model driven by a scripted stand-in teacher (`tools/traintest/mock_teacher_prog.py`), on the CPU. The
following were **not** tried with Qwen3.6 or Gemma 4: answers rendered by the Markdown filter with real text, `/model` switching between two real models
(the reload path is the one `agent_regrow` uses, tried only with the tiny models), `-p` with a real model deciding on its own to use an `--allowed-tools` list,
the line editor on terminals other than the pseudo-terminal of the test, and Windows (the Windows branches are written but were not compiled).
