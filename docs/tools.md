# The agent's second tool set (`src/tools/`)

The katana agent has two tool sets. The first is the original one (`run_command`, `read_file`, `write_file`...,
`src/agent/`). The second lives in `src/tools/`, one file per tool or small family, and is the default since
2026-11-21 (owner's decision, taken before any measurement on the real models); `KATANA_TOOLS=1` goes back to the
first set. Both are always accepted by the dispatcher, so old sessions keep opening; the switch only changes which
set the system prompt declares to the model.

Why a second set: one shell tool that keeps its working directory, whole-file and exact-snippet file tools with a
"read before you change" rule, search with output modes, a task list edited item by item, and the less common tools
announced by name only and loaded on demand (`ToolSearch`), so they cost no context until used.

## Status (2026-11-21)

Every tool has a test script in `tools/tooltest/` that runs it without a model
(`./katana tool <Name> '<json>' [<Name> '<json>' ...]`; several pairs run in one process).

| File | Tools | Checks |
|---|---|---|
| `bash.c` | `Bash`, `TaskStop`, `Monitor` | 49 |
| `read.c`, `write.c`, `edit.c`, `notebookedit.c` | `Read`, `Write`, `Edit`, `NotebookEdit` | 125 |
| `glob.c`, `grep.c` | `Glob`, `Grep` | 161 |
| `regex.c` | the regular-expression engine used by `Grep` | 415 |
| `webfetch.c`, `websearch.c` | `WebFetch`, `WebSearch` | 45 |
| `tasks.c`, `askuserquestion.c`, `planmode.c` | `TodoWrite`, `TaskCreate/Update/List/Get`, `AskUserQuestion`, `EnterPlanMode`, `ExitPlanMode` | 80 |
| `agent.c`, `messages.c`, `workflow.c` | `Agent`, `SendMessage`, `ListAgents`, `SubagentHandback`, `Workflow` | 42 |
| `mcpres.c`, `toolsearch.c`, `skill.c`, `reportfindings.c` | `ListMcpResourcesTool`, `ReadMcpResourceTool`, `WaitForMcpServers`, `ToolSearch`, `Skill`, `ReportFindings` | 71 |
| `worktree.c`, `powershell.c`, `notify.c` | `EnterWorktree`, `ExitWorktree`, `PowerShell`, `PushNotification`, `SendUserFile`, `EndConversation` | 61 |
| `lsp.c` | `LSP` | 39 |
| `schedule.c` | `CronCreate`, `CronDelete`, `CronList`, `ScheduleWakeup` | 156 |

Measured: the 1244 checks pass; `tools/traintest/run.sh` passes, including a whole-loop run with the tiny test model
and the mock teacher with this set (Write, Edit, Bash with the working-directory reset, Grep).

The prompt declares `Bash`, `Read`, `Edit`, `Write`, `Grep`, `Glob`, `TodoWrite`,
`AskUserQuestion`, `WebSearch`, `WebFetch` and `ToolSearch` with full schemas and lists the rest by name. The teacher
takes its tool list from the prompt, so it follows the switch.

The declarations in the prompt are a short form kept in `src/tools/table.c` (`g_ct_short`): the system prompt is
8.8k characters with this set against 7.8k with the first set (measured; it was 19k with the full descriptions).
`ToolSearch` returns the full description and schema of any tool.

Not measured, not done:
- No run with Qwen3.6 or Gemma 4 yet: the effect of this set on the local models is unknown. Next step: codebench
  with `KATANA_TOOLS=1` and without it, alternating.
- Written but not exercised: delivery of scheduled prompts and session mail by the loop, `EndConversation`,
  `SubagentHandback` inside a real sub-agent, `LSP` against a real language server, `PowerShell` with a real `pwsh`.
- The desktop does not show the `NOTIFY`, `FILE` and `FINDING` markers yet.

## Contract

```c
static const char CT_X_DESC[]  = "...";   /* description shown to the model */
static const char CT_X_PROPS[] = "...";   /* JSON schema properties, without the outer braces */
static const char CT_X_REQ[]   = "...";   /* required names */
static void ct_x(const char *js, const char *end, char *out, size_t cap);
```
`src/tools/common.c` has the argument readers (`ct_str`, `ct_int`, `ct_bool`, `ct_has`: top-level keys only) and
the record of files seen in the conversation (`ct_seen_mark`, `ct_seen`). `src/tools/table.c` has the table, the
dispatcher, the declarations for the prompt and the hooks the agent loop calls (`ct_session_reset`, `ct_read_line`,
`ct_end_check`). The result is text in `out`; an error starts with `error: ` and tells the model what to do next.

## The tools

### Bash, TaskStop, Monitor
`Bash(command, timeout, run_in_background, description)`. A new shell per command; the working directory persists
between calls when it stays inside the agent's folder (otherwise it is reset and the result ends with
`Shell cwd was reset to <dir>`); environment variables do not persist. `timeout` in milliseconds, default 120000,
ceiling 600000. At the timeout the command is moved to the background (a command starting with `sleep` is killed)
and the result gives a task id and the output file. Output goes to a file: a successful result comes inline up to
about 30000 characters, above that the file path and a 2000-character preview; a failed result inline up to about
10000, otherwise head and tail. Exit code 1 is not a failure for `grep`, `rg`, `find`, `diff`, `test`, `[`,
`git diff`, `git grep`. `run_in_background` returns at once with a task id; `TaskStop(task_id)` stops it.
`Monitor(command, description, timeout_ms)` starts a command in the background with a deadline (5 min default,
30 max) and returns the output file; lines are not delivered as events yet.

### Read, Write, Edit, NotebookEdit
`Read(file_path, offset, limit, pages)`: lines prefixed with their number; 2000 lines by default; a `PARTIAL view`
notice when the range does not fit; notices for an empty file and an offset past the end; a directory and binary
files are errors with a hint; PDF as text (by `pages` above 10 pages); notebooks as cells. Marks the file as seen.
`Edit(file_path, edits)`: several changes to one file in one call, applied in order in memory, all or none, written once. `edits` is an
array of `{old_string, new_string, replace_all}` or plain text blocks (`<<<<<<<`, old, `=======`, new, `>>>>>>>`, each marker
on its own line, nothing escaped; an empty new part deletes the lines). Up to 64 changes.
`Edit(file_path, old_string, new_string, replace_all)`: exact replacement; the file must have been read in full;
`old_string` must occur once unless `replace_all`; non-UTF-8 files refused; result shows the changed lines; syntax
check and undo as in the first set.
`Write(file_path, content)`: whole file, never appends; overwriting needs a previous read; any size.
`NotebookEdit(notebook_path, cell_id, new_source, cell_type, edit_mode)`: replace, insert, delete; untouched cells
stay byte for byte.

### Glob, Grep
`Glob(pattern, path)`: `**`, `*`, `?`, `[...]`, `{a,b}`; absolute paths, newest first, at most 100.
`Grep(pattern, path, glob, type, output_mode, -i, -n, -A, -B, -C, multiline, literal, head_limit, offset)`: katana's
own backtracking engine (`src/tools/regex.c`) over UTF-8. Syntax: `.` `^` `$` `\A` `\z` `\Z` `\b` `\B` `\<` `\>`;
classes `[...]` with `[[:alpha:]]` and the other POSIX names; `\d` (0-9) `\w` `\s` `\h` `\v` and negations; `\p{L}`
`\p{Lu}` `\p{Ll}` `\p{N}` `\p{Nd}` `\p{P}` `\p{Z}` `\p{Latin}` `\p{Greek}` `\p{Cyrillic}` and `\P{..}` (approximate
tables: Latin, Greek, Cyrillic, Armenian, Hebrew, Arabic, CJK, kana, hangul); escapes `\t \n \r \f \e \a \0 \xHH
\x{H..} \uHHHH`; groups `(...)` `(?:...)` `(?<n>...)` `(?P<n>...)` `(?>...)`; `|`; `* + ? {n} {n,} {n,m}` greedy,
lazy (`?`) and possessive (`+`); lookahead and lookbehind (lookbehind up to 255 characters, no unbounded repetition);
`\1`..`\99`, `\k<n>`; flags `(?imsx)`, `(?-i)`, `(?i:...)`; `\Q...\E`; a `{` that is not a repetition is literal.
Case folding covers accented Latin, Greek and Cyrillic. Each search call has a step budget (10 million plus 100 per
byte); past it the tool returns "pattern too slow on this input". Modes `files_with_matches` (default), `content`,
`count` (also `files`, `lines`, `matches`). Skips binary files and ignored files: `git ls-files` inside a repository,
otherwise the `.gitignore` files of the folder, its parents up to the working folder and its subfolders (`!`, `/`,
`**`, trailing `/`). Forgiving: an invalid pattern is searched as plain text; with zero matches it tries the pattern
as plain text, ignoring case and without `glob`/`type`, and says what it found; `include`, `case_insensitive`,
`ignore_case`, `context`, `regex: false`, `fixed_strings`, `literal` are accepted; a missing `path` names the only
file or folder with that name, if there is one. `KATANA_REGEX_TEST=1` with `subject` and no `path` matches the pattern
against `subject` and prints the spans (used by `tools/tooltest/regex.sh`).

### WebFetch, WebSearch
Web research is paid for in the local model's reading time (about 60 tokens/s) and in rounds, not in network time
(a search is 0.9 s and a page 0.7 s without the model). So both tools return few bytes and one call does the work of
several.

`WebSearch(query | queries, read, allowed_domains, blocked_domains)`: katana's search with a host filter.
- `read` (0 to 5, default 3): the top pages are downloaded at the same time (one `curl` child each, 6 s per page, 10 s
  overall; a page that fails or is slow is only listed), and the result is the numbered list (title, URL, short summary)
  followed by the passages of those pages that match the words of the query, each prefixed with its URL. The whole
  result is about 4,000 bytes (about 1,100 tokens); the budget left after the list is divided among the pages by how
  many query words their passages contain, a page with none gets no space. The pages enter the page memory: a later
  `WebFetch` of the same URL is instant and continues with `offset`. `read=0` returns only the list.
- `queries` (array of up to 4 strings) instead of `query`: the searches run at the same time, results are merged,
  duplicate URLs removed (a URL returned by several queries ranks higher), same byte budget. `query` alone behaves as
  before.
- Pages are never read for local or internal hosts, PDFs, or non-http addresses.

`WebFetch(url, prompt, offset, find, links, render)`: http upgraded to https; hosts without a dot refused (use `curl`
in `Bash` for local servers). Without `offset` the result is short: at most about 4,000 bytes, the passages that match
the words of `prompt`; when none matches (or there is no prompt) the words of the page title and of the most recent
search query; when still nothing, the start of the page. A header line says how long the page is and how to read more.
With `offset`, `find` or `links` the content is exact, as before. An HTTP status of 400 or more is an error (2026-11-22):
the result starts with `error: HTTP 404 (Not Found) from <url>` and carries no page text, the browser fallback is not
tried and nothing is cached. `WebSearch` reading several pages in parallel does not see the status yet.

Both tools end the result with one line, `[web: ...]`, with the network time and the bytes returned (it is also in the
agent log). A result that ends with that line is never sent through the reading notes (`tool_digest`).

Cache across sessions: `$KATANA_CACHE_DIR/web`, or `~/.katana/cache/web`; searches are kept 6 hours, pages (their
converted text and links) 24 hours; over 50 MB the oldest files are removed; `KATANA_WEB_CACHE=0` turns it off.
Repeating a search after a context compaction therefore costs nothing.

Test hooks (used by `tools/tooltest/web.sh`, with a local server and no internet): `KATANA_WEBSEARCH_FIXTURE` (fixed
search results; pages are then read only when `KATANA_WEB_ALLOW_LOCAL=1`), `KATANA_WEB_ALLOW_LOCAL=1` (accept
`http://127.0.0.1` URLs and do not upgrade http).

### Tasks, questions, plan mode
`TodoWrite(todos)` replaces the list; `TaskCreate`, `TaskUpdate`, `TaskList`, `TaskGet` edit it item by item. One
list underneath (the one the desktop card shows). `AskUserQuestion(questions)`: 1 to 4 questions with 2 to 4 options
each, asked one at a time through the existing question mechanism. `EnterPlanMode()` and `ExitPlanMode(plan)`: the
model can enter the read-only mode and present a plan for approval.

### Agents
`Agent(description, prompt)`: a read-only sub-agent with a clean context; only its report returns.
`SendMessage(to, message)`: continues a sub-agent by replay (task, previous report, new message), or leaves a
message for another katana session on this machine (`~/.katana/mail/`). `ListAgents()`. `SubagentHandback(message)`
ends a sub-agent with its report. `Workflow(script | scriptPath | name, args)`: a sequential JSON description of
phases and agents.

### The rest
`LSP(operation, filePath, line, character, query)`: definitions, references, hover, symbols, call hierarchy and
diagnostics from a language server found on PATH (`KATANA_LSP_<EXT>` overrides). `EnterWorktree`/`ExitWorktree`:
an isolated git worktree under `.katana/worktrees/`. `CronCreate`/`CronDelete`/`CronList`/`ScheduleWakeup`:
prompts scheduled inside the session, delivered when the agent is idle. `PushNotification`, `SendUserFile`: local
notification and file hand-over to the interface. `PushNotification` calls `notify-send` only when there is no interface
(`KATANA_UI` unset) and `KATANA_NO_NOTIFY` is unset, empty or `0`; with `KATANA_NO_NOTIFY=1` the result says that
notifications are turned off and that the model should tell the user in its answer (the curriculum generator sets it).
`ExitWorktree` with `remove` ignores untracked `__pycache__/` folders and `*.pyc` files when it checks for unsaved work. `ToolSearch(query)`: returns the declaration of tools listed by
name only. `Skill(skill, args)`: loads instructions from `.katana/skills/<name>/SKILL.md`; a skill in the "ability" format
(`name`, `when`, `needs`, `args`, numbered `steps`, `done`, `report`, `fail`) has its `needs` checked, its steps written as
the task list and `$arg` replaced, and lists its extra files; the system prompt carries one line naming the skills.
`ReportFindings`, `EndConversation`, `PowerShell`, and the MCP resource tools. `Bash`, `Write` and `Edit` obey the `never:`
and `ask:` patterns of `KATANA.md`; the first `Read`, `Edit` or `Write` in a folder with its own `KATANA.md` delivers it once
with the result. MCP calls accept `server__tool` and `mcp__server__tool`; results carry `structuredContent`, errors and a
one-line description of images. Formats, commands and examples: `docs/extending.md`.

katana's own tools without a counterpart in this set stay as they are: `terminal`, `terminal_screen`, `job`,
`read_log`, `howto`, `remember`, `recall`, `install_package`, `download_file`, `find_file`, `list_dir`,
`read_document`, `git`.

## Changes of 2026-11-22 that affect what the model sees

- **Plan-first note.** `loop_plan_note` counts only calls that do something: `Read`, `Grep`, `Glob`, `LSP`, `ToolSearch`,
  `WebSearch`, `WebFetch`, the list and get tools and `AskUserQuestion` are not counted. The note fires on the third and on
  the sixth counted call of a request without a plan. A question answered with reads only never gets it.
- **Unfinished-steps reminder.** `loop_todo_should_nudge` decides. A task made with `TaskCreate` and never moved to
  in progress or completed is a recorded task, not a step of the model's plan: it does not trigger the reminder and the
  engine does not close it at the end of the request.
- **`ToolSearch select:Edit` and `select:AskUserQuestion`** return the same shape the prompt declares: `edits` is a string
  of blocks (an array is still accepted), and a question needs only `question` and `options`, an option only `label`.
- **`howto`** recipes name the current tools (`Read`, `Edit`, `Bash`, `Grep`, `Glob`, `WebSearch`, ...).
- Tests: `tools/tooltest/misc.sh`, `meta.sh`, `files.sh`, `web.sh`, `tasks.sh`, `loopnotes.sh` (`katana toolrun notes` replays a
  list of calls and prints which notes the loop would send), `helpword.sh`.
