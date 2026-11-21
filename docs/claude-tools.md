# Claude Code tools: how each one works, and what katana needs to match them

Goal (owner, 2026-11-21): the katana agent uses the same tools Claude Code uses (same names, same arguments, same
result shape), instead of its own (`run_command`, `read_file`, `write_file`...). Reasons: a Claude Code session then
becomes training data for the local model almost without translation, and the teacher (Claude) is offered the tools it
already knows.

What this document is: the observable behaviour of each tool, taken from the public tools reference
(`code.claude.com/docs/en/tools-reference`, read on 2026-11-21) and from the tool definitions a Claude Code session
shows to the model. It is the interface and the documented algorithm. The implementation in katana is written from
scratch in C; no code of Claude Code is used or was read.

Not measured yet: nothing here was run in katana. The effect of the new names on the local models (Qwen3.6, Gemma 4)
is unknown until codebench is run with old and new names, alternating.

## 1. The whole list, by what we can do with it

| Group | Tools | In katana |
|---|---|---|
| A. Core, local | `Bash`, `Read`, `Write`, `Edit`, `Glob`, `Grep`, `WebFetch`, `WebSearch`, `TodoWrite`, `AskUserQuestion`, `Agent` | all have a katana equivalent; rename and reshape |
| B. Local, katana has a near equivalent | `Monitor`, `TaskStop`, `TaskCreate/Get/List/Update`, `EnterPlanMode`, `ExitPlanMode`, `NotebookEdit`, `ToolSearch`, `Skill` | `job`, `todo`, `/plano`, deferred tools line in the prompt, `howto` |
| C. Local, new work | `LSP`, `EnterWorktree`, `ExitWorktree`, `CronCreate/Delete/List`, `ScheduleWakeup`, `SendMessage`, `ListAgents`, `Workflow`, `PowerShell`, `ListMcpResourcesTool`, `ReadMcpResourceTool`, `WaitForMcpServers`, `EndConversation`, `ReportFindings`, `SubagentHandback` | nothing today (MCP tools exist, resources do not) |
| D. Tied to Anthropic's service | `Artifact`, `RemoteTrigger`, `PushNotification`, `SendUserFile`, `SendFeedback`, `ShareOnboardingGuide`, `ListPlugins`, `ListSkills`, `SearchPlugins`, `SearchSkills`, `SuggestPluginInstall`, `SuggestSkills`, `propose_skills` | cannot exist locally as they are; a local stand-in only where it makes sense (desktop notification, file hand-over in the desktop) |

Two facts from the reference that change the plan:

- On Linux and macOS, Claude Code no longer offers `Glob` and `Grep` by default: Claude searches with `find` and
  `grep` through `Bash`. Sessions recorded on this machine therefore contain mostly `Bash` calls for searching. katana
  should offer `Glob` and `Grep` (the local model benefits from them) and accept that imported sessions use `Bash`.
- `TodoWrite` is the older checklist; newer sessions use `TaskCreate`/`TaskUpdate`/`TaskList`/`TaskGet`, and the
  newest models get neither by default. Imported sessions will contain either or none.

### What the sessions on this machine actually call (measured 2026-11-21)

Counted over the 726 Claude Code session files on this machine (all projects), 15136 tool calls:

| Tool | Calls | Share |
|---|---|---|
| `Bash` | 12624 | 83.4% |
| MCP tools (browser and others) | 829 | 5.5% |
| `Agent` | 415 | 2.7% |
| `Read` | 362 | 2.4% |
| `Write` | 261 | 1.7% |
| `SendMessage` | 242 | 1.6% |
| `Edit` | 146 | 1.0% |
| `Grep` | 82 | 0.5% |
| `TaskStop` | 70 | 0.5% |
| `ToolSearch`, `ListAgents`, `Monitor`, `AskUserQuestion`, `WebSearch`, `Skill` | 96 together | 0.6% |

So `Bash` alone is five calls out of six, and `Bash`, `Read`, `Write`, `Edit`, `Grep` cover 89%. Getting `Bash`
exactly right (working directory, timeout, background, output limits) matters more than everything else together.
`Glob`, `WebFetch`, `TodoWrite` and the Task tools do not appear at all in these sessions.

## 2. Group A: the core

### Bash
Input: `command` (required), `timeout` (milliseconds), `run_in_background` (boolean), `description` (text shown to the
user).

Algorithm:
1. Each command runs in a new non-interactive shell process (the user's shell profile is sourced once and its
   aliases/functions/options reapplied). Environment variables do not survive between calls.
2. The working directory does survive: after the command, the shell's final directory is recorded; if it is inside the
   project (or an added directory) the next command starts there, otherwise it is reset to the project directory and
   the result ends with `Shell cwd was reset to <dir>`. Sub-agents never carry it over.
3. Timeout: default 120 000 ms, ceiling 600 000 ms. At the timeout the command is not killed: it is moved to the
   background (unless it starts with `sleep`) and the result says so, with a task id and the path of the output file.
4. Output goes to a file while the command runs. A successful result comes inline up to about 30 000 characters; above
   that the model gets the file path plus the first 2 000 characters. A failed result comes inline up to about 10 000
   characters, otherwise head and tail of that size. Exit code 1 is not a failure for `grep`, `rg`, `find`, `diff`,
   `test`, `[`, `git diff`, `git grep`.
5. `run_in_background: true` returns at once with a task id; the model is notified when it ends and reads the output
   file with `Read`. `TaskStop` stops it.

katana today (`run_command`, exec.c, jobs.c): `command`, `timeout_s`, `background`; a new shell per command; working
folder kept by the agent (`\x01CWD`); timeout kills the group; output head+tail in memory; `job(id, action)` reads or
stops. Gaps: milliseconds; `run_in_background`; `description`; move-to-background at timeout; output to a file with
path + preview; the exit-code-1 rule; the cwd reset message.

### Read
Input: `file_path` (absolute), `offset` (first line), `limit` (number of lines), `pages` (PDF range such as `1-5`).

Algorithm:
1. Text: returns the lines prefixed with their line number (number, tab, text), from `offset`, at most `limit` lines
   (2000 by default). One call returns at most about 25 000 tokens; a whole file above 256 KB is refused with an error
   telling the model to page with `offset`/`limit` or to search; a whole file above the token limit returns the first
   page with a `PARTIAL view` notice.
2. Empty file: a notice that the file exists and is empty. `offset` past the end: a notice with the line count.
3. Directory: error (list it with `ls` in `Bash`). Binary extension (`.zip`, `.xlsx`...): refused, with a hint.
4. Images are returned as an image; PDFs whole up to 10 pages, otherwise by `pages` (20 at a time, rendered with
   `pdftoppm`); notebooks return all cells with outputs.
5. Side effect: the session records that the file was read (and whether partially). `Edit` and `Write` consult this.

katana today (`read_file`, code.c): `path`, `offset`, `limit`; no line numbers; PDF/HTML as text; the read cursor
(continues where it stopped, keeps notes per chunk). Gaps: `file_path`; line-number prefix; the notices; the read
record for `Edit`/`Write`. The cursor is katana's own and stays: it only acts when neither `offset` nor `limit` is
given, which in Claude's contract means "from the start", so it must be kept behind the same rule (only when the file
is longer than one page).

### Edit
Input: `file_path`, `old_string`, `new_string`, `replace_all`.

Algorithm:
1. The file must have been read in this conversation (a partial read does not count). Viewing it with `cat`, `head`,
   `tail`, `sed -n`, `grep`, `rg` on that single file through `Bash` also counts. (Newer models may skip this; older
   ones may not.)
2. Exact string replacement, no regex, no fuzzy match. `old_string` must occur exactly once, or `replace_all` must be
   true. Zero occurrences or more than one without `replace_all` is an error and nothing is written.
3. If the file changed on disk since the read, the edit still applies when `old_string` matches the current content
   exactly once; the result then warns that the file has other changes.
4. Files that are not valid UTF-8 are refused (the file would be re-saved as UTF-8 and lose bytes).

katana today (`edit_file`): `path`, `old`, `new`, `replace_all`, `sudo`; exact match with uniqueness; a syntax check
after the write; undo. Gaps: argument names; the read-before-edit record; the UTF-8 refusal. The syntax check and undo
stay (they are extra, appended to the result).

### Write
Input: `file_path`, `content`.

Algorithm: creates the file or replaces it whole; never appends. Overwriting an existing file requires that it was
read in this conversation (same rule as `Edit`); new files have no such rule. Content containing U+FFFD is refused when
it would replace a non-UTF-8 file.

katana today (`write_file`): `path`, `content`, `append`, `sudo`; 64 KB limit per call with `append=true` for long
files. Gaps: argument name; the read-before-overwrite rule. `append` has no counterpart: under the new name a long file
is one `Write` (the 64 KB buffer has to grow) or a `Write` plus `Bash` appends. This is the riskiest change for the
local model, which today is told to write long files in parts.

### Glob
Input: `pattern`, `path` (folder, default the working folder).

Algorithm: standard glob with `**` and `{a,b}`; returns paths sorted by modification time, newest first, at most 100,
with a truncation flag when there are more; does not skip gitignored files.

katana today (`glob`): `pattern`, `root`. Gaps: `path`; order by modification time; the cap of 100.

### Grep
Input: `pattern` (regex), `path`, `glob`, `type`, `output_mode` (`files_with_matches` default, `content`, `count`),
`-i`, `-n`, `-A`, `-B`, `-C`, `multiline`, `head_limit`, `offset`.

Algorithm: a regular-expression search (the regex syntax of a Rust-style engine, not POSIX) over the files under
`path`, skipping gitignored files; default output is only the list of files that match; `content` gives the lines
with file and line number and optional context; `count` gives matches per file and a total. An invalid pattern returns
the engine's error.

katana today (`search_files`): `pattern`, `path`, `regex`; literal by default; always returns lines. Gaps: regex by
default; the three output modes; `glob`/`type` filters; context; `head_limit`. If `rg` is installed katana can call
it; otherwise its own search has to cover the modes (POSIX regex is close enough for most patterns, and the
difference has to be stated in the tool description).

### WebFetch
Input: `url`, `prompt` (what to extract).

Algorithm: http is upgraded to https; hosts without a dot (localhost) are refused; the page is fetched (5 min
deadline), HTML converted to Markdown, at most 100 000 characters read; then a separate, small model call answers
`prompt` from that content and only that answer reaches the main model. Responses are cached for 15 minutes. A
redirect to another host is not followed: the result names the target and the model fetches it again.

katana today (`web_fetch`): `url`, `offset`, `find`, `links`, `render`; returns the page text itself; with the digest
on, a sub-agent condenses it. Gaps: `prompt`, and making "answer the prompt from the page" the normal path (it is what
the digest already does, with the task as the prompt). `offset`, `find`, `links`, `render` are katana's own and can
stay as extra arguments.

### WebSearch
Input: `query`, `allowed_domains`, `blocked_domains` (not both).

Algorithm: a search run on Anthropic's backend (up to 8 internal searches per call); returns titles and URLs, not the
pages. katana's `web_search` uses its own sources and returns titles, URLs and summaries. Gaps: only the domain
filters.

### TodoWrite / TaskCreate, TaskUpdate, TaskList, TaskGet
`TodoWrite` input: `todos`, the whole list each time, each item with `content`, `status` (`pending`, `in_progress`,
`completed`) and `activeForm` (the text shown while it is in progress). One item in progress at a time. The Task tools
are the same list edited item by item: `TaskCreate(subject, description)`, `TaskUpdate(taskId, status, ...)`,
`TaskList()`, `TaskGet(taskId)`.

katana today (`todo`): `steps`, one step per line with `[x]` for done; the desktop shows it as a card. Gap: accept the
array form and the item-by-item form, keep one list underneath.

### AskUserQuestion
Input: `questions` (1 to 4), each with `question`, `header` (short label), `options` (2 to 4, each `label` and
`description`), `multiSelect`. The user can always type a free answer. The result is the chosen labels per question.

katana today (`ask_user`): `question`, `options` separated by `|`. Gap: the array form (several questions, labels with
descriptions, multiple choice) in the console and in the desktop dialog.

### Agent
Input: `description` (3 to 5 words), `prompt` (the whole task; the sub-agent sees nothing else), `subagent_type`.

Algorithm: a sub-agent with a clean context runs the task with its own tool calls; the parent receives only its final
report. Sub-agents cannot change the parent's working directory.

katana today (`delegate`): `task`; same idea, one level deep, same model. Gap: argument names. Background agents,
teams and `SendMessage` are group C.

## 3. Group B: local, close to something katana has

- `Monitor`: runs a command in the background and delivers each output line to the model as an event, with a deadline
  (5 min default, 30 max). katana has background jobs that the model polls with `job`; events would need the loop to
  inject a message between turns.
- `TaskStop`: stops a background task by id. Same as `job(id, "stop")`.
- `EnterPlanMode` / `ExitPlanMode(plan)`: plan mode is read-only; leaving it presents the plan for approval. katana
  has plan mode as a user command (`/plano`); the model cannot enter or leave it by itself.
- `NotebookEdit`: `notebook_path`, `cell_id`, `new_source`, `cell_type`, `edit_mode` (`replace`, `insert`, `delete`);
  replacing a code cell clears its outputs. Nothing in katana; it is JSON editing of the `.ipynb`.
- `ToolSearch`: tools are announced by name only and their full definition is loaded on demand. katana's prompt
  already lists the secondary tools in one line without schemas, for the same reason (context).
- `Skill`: loads a named set of instructions into the turn. katana's `howto(topic)` is the same mechanism.

## 4. Order of work

1. Group A under the new names and arguments, selected by a switch (`KATANA_TOOLS=claude`), with the old names still
   accepted by the dispatcher so old sessions keep opening. Results in the shape described above (line numbers in
   `Read`, modes in `Grep`...).
2. Measure: codebench on Qwen3.6 and Gemma 4, old names against new names, alternating. The switch becomes the default
   only if the new names are not worse.
3. Teacher mode offers the same set, so the teacher's calls and the local model's calls have one vocabulary.
4. Converter from Claude Code session files to training data (now mostly a copy: same names, same arguments).
5. Group B, then group C, in the order the recorded sessions actually use them (count the calls in the session files
   first).

katana's own tools without a counterpart stay as they are: `terminal`, `terminal_screen`, `job`, `read_log`, `howto`,
`remember`, `recall`, `install_package`, `download_file`, `find_file`, `list_dir`, `read_document`, `git`.
