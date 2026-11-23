# Tool set review (2026-11-23)

Scope: a review of the agent's tool set, from the data that exists (agent log, desktop sessions, curriculum v7) and from the
code (`src/tools/`, `src/agent/prompt.c`, `src/agent/dispatch.c`). **No code was changed, nothing new was trained or
measured** (owner's decision for this phase). Every number below was counted from an existing file or read from the code;
the only program run was `katana agentsys` and `katana tool ToolSearch`, to count the characters of the declarations (a size
count, not a performance measurement). Where a statement is an opinion it is marked **(opinion)**; where it is a fact from a
file it carries the number.

Related: `docs/uioe-code-audit.md` (weak point 4: two tool sets), `docs/research/uioe-code-agents.md` (sections 2, 7, 12),
`docs/research/uioe-context-memory-routing.md` (section 6), `docs/tools.md`, `docs/curriculum.md`,
`docs/code-index-lifecycle.md` (the `Symbols tree` design).

## 0. Summary

1. **There are 46 tools in the table (`src/tools/table.c`) plus 24 names of the old set still accepted by the dispatcher**
   (`src/agent/dispatch.c`, name-based `strcmp` chain; `agent_tool_run` tries the table first, then the old names). The prompt
   declares 12 of the 46 (`core[]` + `web[]` + `ToolSearch`; `Skill` is added when skills exist), 4893 characters in the
   short form. For a model trained with `--no-tool-decl` (the working model) **the declaration block is not in the prompt at
   all**: the prompt is 5270 bytes without it and 10747 bytes with it (`katana agentsys`, `KATANA_TOOL_DECL=0|1`). So for the
   working model "declared" means "taught in volume by the curriculum and named in the rules text", not "present in the prompt".
2. **Real use is tiny and concentrated.** The only calls that the owner's criterion attributes to the model on 2026-11-23 are
   **43** (Read 18, TodoWrite 12, Find 7, ToolSearch 4, Glob 1, Grep 1); in four desktop sessions. `Symbols` has **0** calls in
   any real session, in every source I could read (log, 8 desktop sessions, 6 CLI sessions). That is a small sample (one topic,
   one afternoon), so it shows "never needed in these sessions", not "useless".
3. **The curriculum is the real tool vocabulary.** v7 train has 3349 calls in 1042 conversations. Fourteen tools carry 91.9%
   of them (TodoWrite 20.2%, Read 19.3%, Bash 13.3%, Find 7.9%, Symbols 7.6%, Grep 7.4%, Edit 5.3%, ToolSearch 4.2%, Glob 3.0%,
   then Write, Skill, WebFetch, WebSearch, AskUserQuestion under 1% each). **Rename, Replace and Patch have 0 examples** and
   are not even a `ToolSearch` target in the curriculum, so the model cannot know their arguments (they are named in the
   "More tools exist" line only). `Symbols` is taught on 7 of its 10 actions: `refs`, `uses` and `flow` have **0 examples**
   (`callers` 117, `impact` 38, `callees` 35, `def` 27, `outline` 22, `at` 9, `deps` 5).
4. **Recommendation in one line:** keep the central list at 12, make it `Bash, Read, Edit, Write, Find, Symbols, Grep,
   TodoWrite, AskUserQuestion, WebSearch, WebFetch, ToolSearch` (Symbols in, Glob out to a by-name tool), retire the old
   24 names in stages (section 7), do not teach more tools in v8 (section 8), and put one decision rule between Find and
   Symbols (section 6.2).
5. **What not to change without retraining** is in section 9; the short version: names and argument keys of every tool the
   curriculum teaches (TodoWrite `todos`/`done`, Read `file_path`/`offset`/`limit`, Edit `old_string`/`new_string`, Find `query`/
   `mode`/`path`, Symbols `action`/`name`/`file`/`line`/`depth`/`path`, Grep `pattern`/`output_mode`/...).

## 1. Data and method

### 1.1 Sources

| source | what it is | size |
|---|---|---|
| `~/.katana/agente.log` | one line per tool call: `date TAB tool TAB arguments TAB result` (arguments cut at 400 chars, result at 200: `agent_log`) | 9695 lines, 2026-11-16 to 2026-11-23 |
| `~/.katana/desktop/sessoes/*.json` | desktop sessions: each tool call is a message with `nome` and `estado` | 8 sessions, 759 calls |
| `~/.katana/desktop/sessoes/*.ksess`, `~/.katana/sessions/*/*.ksess` | raw conversations; tool calls counted as `<function=Name>` | 14 files |
| `~/.katana/api-sessions/` | does not exist on this machine | 0 |
| `data/curriculum/v7/v7-train.jsonl.gz` | curriculum v7, 1042 conversations, 3349 tool calls (validation: 239 conversations, 1141 calls) | |
| `katana agentsys`, `katana tool ToolSearch` | size of the prompt and of each declaration | |

### 1.2 How the log was split

The owner's criterion: a line of 2026-11-23 whose arguments contain `"arguments":` came from the model (the XML call parser
`tool_xml_parse` rewrites a call into `"arguments": {...}` before the tool runs); the other lines are tests by the agents
(`katana tool ...` passes the raw JSON, without the wrapper).

- **A, model, 2026-11-23:** 43 lines.
- **B, tests, 2026-11-23:** 131 lines (Find 86, Symbols 30, Read 8, ToolSearch 3, find_file 2, Glob 1, Bash 1). 59 of the 86
  Find lines are repeats of an identical call: they are test loops, not behaviour. Excluded.
- **C, earlier days with `"arguments":`:** 3397 lines (2026-11-16 to 2026-11-22). They have the model's call format, but the
  period mixes the old set and the table set (default only from 2026-11-21), other models and
  different prompts (the log does not record the model), and `katana toolparse` also writes this format. I show them as context, not as evidence about the
  current model. Their top tools are `run_command` 815, `read_file` 491, `Bash` 459, `edit_file` 371, `TodoWrite` 205,
  `write_file` 192, `Read` 158, `list_dir` 148: the era of the old set.
- **D, earlier days without `"arguments":`:** 6124 lines, 3558 of them `Grep` (3049 repeated): `tools/tooltest/*.sh` loops. Excluded.

Caveat that matters: the criterion is a signal, not proof. The 43 lines match the desktop sessions of the same day (40 calls:
Find 7, Read 18, TodoWrite 12, Grep 2, Glob 1; `ToolSearch` is not shown by the desktop), so they do look like the real calls.

"Errors" in the tables: the result starts with `error` (or is `No matching tools`). "Rep" is a call whose tool and arguments
had already appeared in the same set.

### 1.3 What the 43 real calls show

- **TodoWrite 12, 3 errors, 6 repeats.** All 3 errors are the same: `{"done": "1"}` sent before any list existed
  (`error: there is no task list yet; send todos (one step per line) first`). The `done` shortcut is half of the curriculum's
  TodoWrite calls (436 `done` against 241 `todos` argument uses) and it fails when the model forgets the first call.
- **Read 18, 1 error:** a path invented from the user's words (`/home/alexandre/mnt/dados/greymod/README.md` at 08:09; the real folder
  is `/mnt/dados/source/greymod`). The same wrong path appears again in the test lines of set B (08:22 to 09:02, five times), which is
  how the agents reproduced it. The engine now adds the `[katana: you mention "greymod" ...]` note (seen in
  `20261123-101937-867.ksess`), after which that session went straight to the right folder.
- **Find 7, 0 errors, 3 repeats:** the same two queries re-issued ("katana sends session info to the phone"; the Portuguese
  one). Find is the only code-navigation tool the model chose.
- **ToolSearch 4, 1 error, 2 repeats:** `select:katana` and three times `katana` (07:47 to 07:50): in all 4 calls the model searched for a
  project name as if it were a tool. The curriculum teaches `select:ToolName` for a tool name; none of the 4 real calls did that.
- **Glob 1, Grep 1, Symbols 0, Bash 0, Edit 0, Write 0** in this set. The sessions of the day were questions about a project
  (read-only), which is why Bash, Edit and Write are absent; this is not evidence about them.

## 2. Usage by tool

Columns: **log** = set A (model, 2026-11-23, n=43) with errors and repeats; **desk** = all desktop sessions (n=759, mostly the
old set until 2026-11-21); **old log** = set C (n=3397); **curr** = calls in v7 train (n=3349), share, conversations that use it,
and results that start with `error` in the curriculum (replayed by `katana curriculum check`; many are designed failure cases).

| tool | log | err | rep | desk | old log | curr | % | convs | curr err |
|---|---|---|---|---|---|---|---|---|---|
| Bash | 0 | 0 | 0 | 173 | 459 | 444 | 13.3 | 297 | 2 |
| TaskStop | 0 | 0 | 0 | 1 | 3 | 8 | 0.2 | 8 | 1 |
| Monitor | 0 | 0 | 0 | 0 | 0 | 7 | 0.2 | 7 | 2 |
| Read | 18 | 1 | 2 | 42 | 158 | 648 | 19.3 | 420 | 23 |
| Write | 0 | 0 | 0 | 12 | 88 | 26 | 0.8 | 25 | 0 |
| Edit | 0 | 0 | 0 | 10 | 49 | 177 | 5.3 | 105 | 4 |
| NotebookEdit | 0 | 0 | 0 | 0 | 0 | 6 | 0.2 | 6 | 1 |
| Glob | 1 | 0 | 0 | 4 | 16 | 99 | 3.0 | 94 | 2 |
| Grep | 1 | 0 | 0 | 2 | 4 | 248 | 7.4 | 192 | 0 |
| Find | 7 | 0 | 3 | 7 | 0 | 263 | 7.9 | 195 | 4 |
| Symbols | 0 | 0 | 0 | 0 | 0 | 253 | 7.6 | 183 | 0 |
| Rename | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0 |
| Replace | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0 |
| Patch | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 | 0 | 0 |
| WebFetch | 0 | 0 | 0 | 23 | 26 | 28 | 0.8 | 25 | 8 |
| WebSearch | 0 | 0 | 0 | 22 | 39 | 25 | 0.7 | 25 | 0 |
| TodoWrite | 12 | 3 | 6 | 30 | 205 | 677 | 20.2 | 219 | 1 |
| TaskCreate | 0 | 0 | 0 | 0 | 0 | 13 | 0.4 | 11 | 1 |
| TaskUpdate | 0 | 0 | 0 | 0 | 0 | 5 | 0.1 | 5 | 1 |
| TaskList | 0 | 0 | 0 | 0 | 0 | 5 | 0.1 | 5 | 0 |
| TaskGet | 0 | 0 | 0 | 0 | 0 | 5 | 0.1 | 5 | 1 |
| AskUserQuestion | 0 | 0 | 0 | 14 | 14 | 23 | 0.7 | 23 | 0 |
| EnterPlanMode | 0 | 0 | 0 | 0 | 0 | 5 | 0.1 | 5 | 0 |
| ExitPlanMode | 0 | 0 | 0 | 0 | 0 | 6 | 0.2 | 5 | 1 |
| Agent | 0 | 0 | 0 | 0 | 0 | 5 | 0.1 | 5 | 3 |
| EnterWorktree | 0 | 0 | 0 | 0 | 0 | 5 | 0.1 | 5 | 1 |
| ExitWorktree | 0 | 0 | 0 | 0 | 0 | 5 | 0.1 | 5 | 1 |
| PowerShell | 0 | 0 | 0 | 0 | 0 | 3 | 0.1 | 3 | 3 |
| LSP | 0 | 0 | 0 | 0 | 0 | 6 | 0.2 | 6 | 2 |
| CronCreate | 0 | 0 | 0 | 0 | 0 | 7 | 0.2 | 6 | 0 |
| CronDelete | 0 | 0 | 0 | 0 | 0 | 5 | 0.1 | 5 | 2 |
| CronList | 0 | 0 | 0 | 0 | 0 | 5 | 0.1 | 5 | 0 |
| ScheduleWakeup | 0 | 0 | 0 | 0 | 0 | 5 | 0.1 | 5 | 0 |
| PushNotification | 0 | 0 | 0 | 0 | 0 | 5 | 0.1 | 5 | 1 |
| SendUserFile | 0 | 0 | 0 | 0 | 0 | 5 | 0.1 | 5 | 1 |
| EndConversation | 0 | 0 | 0 | 0 | 0 | 5 | 0.1 | 5 | 1 |
| ListMcpResourcesTool | 0 | 0 | 0 | 0 | 0 | 5 | 0.1 | 5 | 1 |
| ReadMcpResourceTool | 0 | 0 | 0 | 0 | 0 | 6 | 0.2 | 5 | 3 |
| WaitForMcpServers | 0 | 0 | 0 | 0 | 0 | 5 | 0.1 | 5 | 0 |
| ToolSearch | 4 | 1 | 2 | 0 | 6 | 140 | 4.2 | 140 | 0 |
| Skill | 0 | 0 | 0 | 0 | 1 | 26 | 0.8 | 25 | 0 |
| ReportFindings | 0 | 0 | 0 | 0 | 0 | 5 | 0.1 | 5 | 1 |
| SendMessage | 0 | 0 | 0 | 0 | 0 | 6 | 0.2 | 6 | 4 |
| ListAgents | 0 | 0 | 0 | 0 | 0 | 5 | 0.1 | 5 | 0 |
| SubagentHandback | 0 | 0 | 0 | 0 | 0 | 5 | 0.1 | 5 | 5 |
| Workflow | 0 | 0 | 0 | 0 | 0 | 5 | 0.1 | 5 | 3 |
| *old set* | | | | | | | | | |
| run_command | 0 | 0 | 0 | 183 | 815 | 0 | 0 | 0 | 0 |
| read_file | 0 | 0 | 0 | 20 | 491 | 0 | 0 | 0 | 0 |
| write_file | 0 | 0 | 0 | 20 | 192 | 0 | 0 | 0 | 0 |
| edit_file | 0 | 0 | 0 | 130 | 371 | 0 | 0 | 0 | 0 |
| list_dir | 0 | 0 | 0 | 5 | 148 | 0 | 0 | 0 | 0 |
| find_file | 0 | 0 | 0 | 2 | 5 | 0 | 0 | 0 | 0 |
| search_files | 0 | 0 | 0 | 0 | 30 | 0 | 0 | 0 | 0 |
| glob | 0 | 0 | 0 | 0 | 3 | 0 | 0 | 0 | 0 |
| web_search | 0 | 0 | 0 | 0 | 75 | 0 | 0 | 0 | 0 |
| web_fetch | 0 | 0 | 0 | 1 | 36 | 0 | 0 | 0 | 0 |
| read_document | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| terminal | 0 | 0 | 0 | 0 | 73 | 9 | 0.3 | 6 | 3 |
| terminal_screen | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| job | 0 | 0 | 0 | 2 | 7 | 0 | 0 | 0 | 0 |
| delegate | 0 | 0 | 0 | 0 | 2 | 0 | 0 | 0 | 0 |
| ask_user | 0 | 0 | 0 | 9 | 9 | 0 | 0 | 0 | 0 |
| todo | 0 | 0 | 0 | 46 | 55 | 0 | 0 | 0 | 0 |
| git | 0 | 0 | 0 | 0 | 1 | 12 | 0.4 | 7 | 0 |
| install_package | 0 | 0 | 0 | 0 | 0 | 6 | 0.2 | 6 | 2 |
| download_file | 0 | 0 | 0 | 0 | 0 | 6 | 0.2 | 6 | 2 |
| read_log | 0 | 0 | 0 | 0 | 0 | 5 | 0.1 | 5 | 2 |
| remember | 0 | 0 | 0 | 0 | 5 | 7 | 0.2 | 6 | 1 |
| recall | 0 | 0 | 0 | 1 | 4 | 5 | 0.1 | 5 | 0 |
| howto | 0 | 0 | 0 | 0 | 0 | 5 | 0.1 | 5 | 0 |
| MCP tools (`server__tool`, 20+ names) | 0 | 0 | 0 | 0 | 7 | 54 | 1.6 | | |

Reading the table:

- **Eight old-set names are still taught by v7** (`terminal` 9, `git` 12, `remember` 7, `install_package` 6, `download_file` 6,
  `read_log` 5, `recall` 5, `howto` 5: 55 calls, 1.6%). They are the ones named in the prompt's "Also:" line
  (`prompt.c`, the `More tools exist` paragraph). **The 16 other old names are not taught at all** (`run_command`,
  `read_file`, `write_file`, `edit_file`, `list_dir`, `find_file`, `search_files`, `glob`, `web_*`, `read_document`, `job`,
  `delegate`, `ask_user`, `todo`, `terminal_screen`): v7 was regenerated for the table set, and they are accepted only because the
  dispatcher keeps them.
- **The old set dominates the older real use** (set C and the 2026-11-20 desktop session: `run_command` 183, `edit_file` 130,
  `todo` 46). Those sessions were made before the table set became the default (2026-11-21, `prompt.c` comment).
- **Curriculum errors.** `SubagentHandback` 5 of 5 return an error on purpose (it is a sub-agent-only tool, called outside one);
  `SendMessage` 4 of 6, `PowerShell` 3 of 3 (not installed), `Agent` 3 of 5 and `Workflow` 3 of 5 (examples of the failure
  paths). `Read` 23 errors of 648 and `WebFetch` 8 of 28 are the large ones in absolute terms and are the "file does not exist"
  and "URL refused" lessons.
- **Task* tools (TaskCreate/Update/List/Get) are in the skip list** of the "More tools" line (`prompt.c`: "the same list as
  TodoWrite with more calls; they stay in the dispatch (owner's decision, 2026-11-22)"). The model only knows them from the
  curriculum (28 calls, 11 conversations on TaskCreate, `select:TaskCreate` 11 times).

## 3. What each declaration costs

Measured from the program: `katana agentsys` (system prompt) and `katana tool ToolSearch '{"query":"select:Name"}'` (one
declaration per call; this returns the **long** form). Characters, newline included; tokens are about characters / 4 **(opinion;
the tokenizer was not run)**.

### 3.1 The prompt, today

| | bytes |
|---|---|
| prompt without declaration block (what the specialized model sees; `KATANA_TOOL_DECL=0`) | 5270 |
| prompt with the block (`KATANA_TOOL_DECL=1`, base models, Gemma 4 path) | 10747 |
| difference (the block: 12 declarations, the `# Tools` header and the call-format instructions) | 5477 |
| the 12 declarations alone, short form | 4893 |
| "More tools exist ..." paragraph (29 names plus the old "Also:" line) | 805 |
| the Find/Grep/Glob/Symbols/Read rule paragraph | 811 |
| the whole rules-and-tools part (from "You are katana" to "# Environment") | 4086 |

### 3.2 Short form (what is declared today; `g_ct_short` in `table.c`)

| tool | chars | | tool | chars |
|---|---|---|---|---|
| Edit | 613 | | WebFetch | 303 |
| Grep | 581 | | ToolSearch | 301 |
| Bash | 541 | | WebSearch | 302 |
| AskUserQuestion | 500 | | Write | 289 |
| Find | 490 | | Glob | 277 |
| TodoWrite | 354 | | Read | 342 |

### 3.3 Long form (what `ToolSearch` returns, and what `ct_decl` uses for anything without a short form)

Largest: Grep 3436, Symbols 2743, Edit 1875, Replace 1812, WebFetch 1718, Rename 1580, WebSearch 1508, Read 1505, LSP 1424,
NotebookEdit 1408, Find 1406, CronCreate 1335, Bash 1333, ReportFindings 1278, AskUserQuestion 1288. Smallest: TaskList 194,
CronList 271, TaskGet 293, CronDelete 300, EnterPlanMode 363, SubagentHandback 404, TaskStop 433. The other 24 are between 457 and 1095.

**All 46 in the long form: 45,935 characters, about 11k tokens (opinion).** The same 12 declared ones in the short form are 4893.
`docs/tools.md` already records the same effect ("19k with the full descriptions, 8.8k short form" of the older, whole prompt).
Per `ToolSearch` fetch the cost is the long form of the tool (194 to 3436 characters), paid once per session per tool, and a
`select:` of several names in one call costs the sum.

Two facts the sizes expose:

- `Symbols` is the second most expensive declaration (2743) and **has no short form**, which is part of why it is not in
  `core[]`. A short form of about 500 characters like Find's would fit it in the central list.
- The short forms drop all argument descriptions (`Find` short has `query, path, limit, mode` with no text); for the working
  model this is irrelevant (it has no declarations), for a base model it is the only description it gets.

## 4. Overlaps, from the code

### 4.1 Code navigation: Find, Grep, Glob, Symbols, LSP (and the old find_file, search_files, glob, list_dir)

| tool | question it answers | arguments | output and limits | index |
|---|---|---|---|---|
| **Find** (`find.c`, `findgraph.c`) | "where is this in the code", from words and/or identifiers | `query`, `path`, `limit` (default 6, max 12), `mode` = `search` (default) or `refs` | candidates best first: `file:first-last`, symbols defined there, best lines, other files that reference those symbols; `[exact]`/`[probable]` or `[confidence N level]`; a name or a "quoted text" is checked exactly and answered `NOT FOUND`; refuses the home folder as a project (`error: the working folder is your home folder`) | Find `.idx` (`.katana/cache/find-<hash>.idx`) |
| **Symbols** (`symbols.c` -> `symtool.c`) | structural facts about a **known** name or file | `action` (10: def, refs, callers, callees, uses, outline, at, deps, impact, flow), `name`, `file`, `line`, `limit` (max 100), `depth` (impact, max 6), `path` | `file:lines`, kind, `[confidence N level; how found]`; tries other spellings; `NOT FOUND` states what was indexed; blind to macro-generated names and calls through pointers | coder store (`<folder>/store/`), refreshed before each call |
| **Grep** (`grep.c`, `regex.c`) | exact text or regex in file contents | `pattern`, `path`, `glob`, `type`, `output_mode` (files_with_matches default / content / count), `-i -n -A -B -C context multiline literal head_limit offset` (15 properties) | own regex engine; no index; the long declaration is 3436 chars because it documents the syntax | none |
| **Glob** (`glob.c`) | file names by pattern | `pattern`, `path` | absolute paths, newest first, max 100; gitignored files are NOT skipped | none |
| **LSP** (`lsp.c`) | language-server answers | `operation` (10), `filePath`, `line`, `character`, `query` | `path:line:col`; needs the server (clangd, pyright, ...) installed and a 1-based position | external server |
| old `find_file` | folder or file by part of the name **anywhere on the machine** | `name`, `under`, `kind` | path list with kind and date | none |
| old `search_files` | text in files under a folder | `pattern`, `path`, `regex` | simple, 3 arguments | none |
| old `list_dir` | a folder with sizes and dates | `path` | the only tool that lists a directory (table `Read` says "It cannot list directories: use Bash with ls") | none |

Differences that are real and not cosmetic:

1. **Find and Symbols answer different questions with different indexes.** Find `mode=refs` and `Symbols refs` overlap
   (both return definition plus uses, each use with its enclosing function); the audit already names the two-index split
   (`uioe-code-audit.md`, weak point 1). The curriculum teaches only `Find refs` (68 calls) and never `Symbols refs` (0).
2. **Grep and Find overlap on literal text**: `Find "quoted text"` is checked exactly in the files and says NOT FOUND; Grep with
   `literal=true` does the same without an index and without the ranking. The rule in the prompt ("Find first; Grep only for an
   exact pattern") resolves it, and the curriculum agrees: Grep 248 calls with `output_mode` in 218 of them.
3. **Glob has no unique capability that Find lacks except "list by name pattern with `**`, newest first"**. In `Find`, a
   file-name query is answered from the index, not by pattern. Glob is a cheap, deterministic tool; its cost is one more name
   to choose between (3.0% of curriculum calls, 1 of 43 real calls).
4. **LSP is dominated by Symbols in the code available here.** LSP needs an external server per language and a position;
   Symbols needs only a name. LSP adds `hover`, `diagnostics` and `goToImplementation`, which Symbols does not have. Curriculum 6 calls, real 0.
5. **Machine-wide location has no home in the table set.** `find_file` (old) finds a folder anywhere; `Find` refuses the home
   folder, `Glob` needs a `path`, and the engine's place note (`[katana: you mention "greymod" ... It is at: ...]`) covers only
   a name the user wrote. Evidence: on 2026-11-23 the model sent a wrong path to a folder it could not locate (log set A, 08:09) and the
   test lines of set B repeat it five times, while the old `find_file` found the folder in one call (`find_file {"name":"greymod","kind":"dir"}` at 08:23).
   This gap must be closed before `find_file` is retired (section 7).
6. **Directory listing**: only `list_dir` (old) and `Bash ls`. The old log shows `list_dir` 148 calls, 100 of them repeats
   (set C: it was a heavily used first step). The table set answers with `Bash ls` or the environment block in the prompt
   (folders around you).

### 4.2 Reading: Read, read_file, read_document, Find's snippets

| tool | arguments | behaviour |
|---|---|---|
| **Read** (`read.c`) | `file_path`, `offset`, `limit`, `pages` | line numbers with tab; 2000 lines default; a **large source file read without offset/limit returns a symbol map plus the first 40 lines** (`ct_outline`); PARTIAL notice; PDFs (`pages`, max 20) and notebooks; read-before-edit bookkeeping (`ct_seen`); delta notes for a repeated read (`rd_delta`); subfolder `KATANA.md` delivered once |
| old **read_file** | `path`, `offset`, `limit` | text file, PDF and HTML as text; no symbol map, no read-before-edit tracking |
| old **read_document** | `path` | PDF (`pdftotext`), HTML, text |

`Read` fully contains both old tools except HTML-as-text (WebFetch does HTML for URLs; a local `.html` is read raw). Curriculum
`Read`: 648 calls, `offset` in 240 and `limit` in 242; the cursor family (`x-cursor`, 88 `Read`) teaches the continuation.

### 4.3 Shell: Bash, run_command, terminal, PowerShell, Monitor, TaskStop, job

| tool | stateful? | arguments | notes |
|---|---|---|---|
| **Bash** | cwd carries over, environment does not | `command`, `description`, `timeout` (ms), `run_in_background` | stdin closed; at timeout moves to background and returns a task id and output file; large output saved to a file with a preview |
| old **run_command** | cwd | `command`, `timeout_s`, `background` | same job, different key names; background returns a job id read with `job` |
| old **terminal** | **yes: a pty session** | `action` (open, send, read, close, list), `name`, `text` | the only tool that keeps a shell alive (interactive programs, REPLs); max sessions `TERM_MAX`; companion `terminal_screen` |
| **PowerShell** | no | `command`, `timeout`, `run_in_background`, `description` | `pwsh` only; fails when absent; 3 curriculum calls, all errors |
| **Monitor** | background | `command`, `description`, `timeout_ms` | watch a log; read with Read; stop with TaskStop |
| **TaskStop** / old **job** | | `task_id` / `id`, `action` | stop and read background work |

Real: `Bash` is 459 calls in set C and 173 in the desktop (all before 2026-11-22); 0 in the 43 model calls of 2026-11-23 (read-only
questions). The old `terminal` (73 calls in set C, 33 repeats in set D) is the only stateful shell and **has no table equivalent**;
it is taught in v7 (9 calls). `PowerShell` and `Monitor` can both be written as `Bash` (Monitor = `run_in_background` plus `Read` on the
output file).

### 4.4 Changing files: Edit, edit_file, Write, write_file, Rename, Replace, Patch, NotebookEdit

| tool | what is different |
|---|---|
| **Edit** | `file_path`, `old_string`, `new_string`, `replace_all`, `edits` (several blocks, text format `<<<<<<< / ======= / >>>>>>>`, all or none); exact match, unique unless `replace_all`; read-before-edit; syntax check after; accepts a file changed on disk (audit K5). 177 curriculum calls (`replace_all` 38, `edits` 6) |
| old **edit_file** | `path`, `old`, `new`, `replace_all`, `sudo`; `sudo=true` writes as root (password dialog) |
| **Write** | whole file; refuses an existing unread file; never appends; no `sudo` argument (the long text says "use Bash with sudo") |
| old **write_file** | `path`, `content`, `append`, `sudo`; 120-line parts with `append=true` was the old way of writing a long file, `Write` replaces it ("write a long file in one call") |
| **Rename** | text rename of a whole word in all code/script/config files; `dry_run` default true; refuses if `to` exists; not language-aware (members and locals with the same name change too) |
| **Replace** | regex or literal multi-file replace with `\1` groups, `dry_run` default true, `limit_files` 100 |
| **Patch** | unified diff, many files, create/delete, all or nothing, line numbers may be off by 30 |
| **NotebookEdit** | one cell of an `.ipynb` (replace/insert/delete) |

What is unique: `sudo` writes exist only in the old pair (table set expects `Bash` + `sudo`); `Rename`, `Replace` and `Patch` are
**the only multi-file mutation tools and have 0 curriculum examples and 0 real calls**. The `x-rename` family (16 train, 7
validation conversations) teaches the manual way (Grep 28, Read 42, Edit 41, TodoWrite 50, Bash 17, Glob 3) for the very request
`Rename` answers. So the data contradicts the tool list: either the family should teach `Rename`, or `Rename` should go **(opinion:
decide in v8, section 8; today the model does what `x-rename` shows)**.

### 4.5 Web: WebSearch, WebFetch, web_search, web_fetch

| tool | arguments | notes |
|---|---|---|
| **WebSearch** | `query` or `queries` (up to 4 at once), `read` (default 3 pages downloaded), `allowed_domains`, `blocked_domains` | with `read=3` the result already holds the matching passages of the top pages: "you usually do not need WebFetch afterwards" |
| **WebFetch** | `url`, `prompt`, `offset`, `find`, `links`, `render` | reads one page, passages for the prompt first; localhost refused (use curl in Bash) |
| old **web_search**, **web_fetch** | `query` / `url, offset, find, links, render` | same engines behind (`websearch.c`/`web.c`), old key names |

The new pair is a strict superset of the old pair. In real use the model calls them 22 and 23 times (desktop) and the curriculum
25 and 28. WebFetch is the one with the highest curriculum error rate (8 of 28), mostly "this URL is a PDF" and refusals.

### 4.6 Plans and tasks: TodoWrite, TaskCreate/Update/List/Get, old todo, EnterPlanMode/ExitPlanMode

| tool | model |
|---|---|
| **TodoWrite** | `todos` (text, one step per line, marks `[ ] [>] [x]`) and/or `done` (N, the number of the finished step); the engine answers with the next step ("Step 2 of 3: ..."), and `ct_todo_autofinish` closes the last step (`loop.c:988`) |
| **TaskCreate/Update/List/Get** | an id-addressed task list; one tool call per change |
| old **todo** | `steps` text with `[x]` marks, same job as TodoWrite |
| **EnterPlanMode / ExitPlanMode** | a read-only lock for the engine (`g_plan_mode`); `ExitPlanMode(plan)` asks the user |

Numbers: TodoWrite is 20.2% of curriculum calls and 12 of 43 real calls (28%), with 3 errors, all the missing first list
(section 1.3). Task* total 28 curriculum calls and 0 real. The two lists show up the same way in the desktop (`backend.cpp` treats
`TodoWrite`, `todo`, `Task*` as the same progress card). **The Task* family is a second way to write a plan that the model never
chose.**

### 4.7 Agents: Agent, delegate, SendMessage, ListAgents, SubagentHandback, Workflow

| tool | what it does |
|---|---|
| **Agent** | one sub-agent, clean context, read-only (plan-mode lock), `SUB_ROUNDS 12`, `SUB_GEN 1200`, only the final report returns |
| old **delegate** | the same engine (`delegate.c`) with a `task` argument |
| **SendMessage / ListAgents** | continue a sub-agent (re-run with its task and last report) or leave a message in another katana session's mailbox |
| **SubagentHandback** | only inside a sub-agent: deliver the final report (5/5 curriculum calls outside one are errors by design) |
| **Workflow** | sequential JSON phases of sub-agents (no JavaScript, nothing in the background) |

Real calls: 0 (log, desktop, sessions). Because the model is one and the KV cache is swapped, sub-agents run **one at a time** and a
sub-agent costs a full re-prefill (audit S5, weak point 8); `ListAgents` says "sub-agents here run one at a time and are always
finished". For a 35B-A3B local model the whole group is a heavy primitive **(opinion)**.

### 4.8 Memory and reports: remember, recall, ReportFindings, facts

| tool | what it does |
|---|---|
| old **remember** / **recall** | one line `date TAB fact` in `~/.katana/memoria.txt`; exact-duplicate check; recall = word-hit count over at most 4096 (audit M1). The last 20 facts are injected in the system prompt unconditionally. Taught in v7 (remember 7, recall 5), not declared, named in the "Also:" line |
| **ReportFindings** | a typed review list (file, summary, failure_scenario); a formatter, it does not search or store; 5 curriculum calls, 1 error, 0 real |

They do not overlap: `remember`/`recall` is the only durable memory; `ReportFindings` is a structured answer format for a review
and could be a convention in the answer text **(opinion)**.

### 4.9 Environment and messaging tools (no overlap, by-name by nature)

CronCreate/Delete/List and ScheduleWakeup (session-only schedules; `ScheduleWakeup` is one pending wakeup, `CronCreate` many),
PushNotification (desktop notification), SendUserFile (hand a file to the desktop), EndConversation, the three MCP resource tools
(`ListMcpResourcesTool`, `ReadMcpResourceTool`, `WaitForMcpServers`), EnterWorktree/ExitWorktree, Skill, git, download_file,
install_package, read_log, howto. `Cron*` and `ScheduleWakeup` overlap with each other (both deliver a prompt later); they are 7+5+5+5 =
22 curriculum calls, 0 real.

## 5. Recommendation per tool

Actions: **DECLARE** (central list; short form in the prompt for base models; taught in volume), **BY NAME** (named in the
"More tools" line, reached with `ToolSearch select:Name`), **FOLD** (becomes an action or mode of another tool), **RETIRE** (old set:
keep accepting, stop naming, stop teaching), **REMOVE** (delete the tool).
"Cost" is the declaration size from section 3 (short if it exists, otherwise long).

| tool | action | reason (numbers) | cost chars |
|---|---|---|---|
| Bash | DECLARE | 13.3% of curriculum; 459 + 173 old real calls; the universal escape hatch; absorbs PowerShell/Monitor/run_command | 541 |
| Read | DECLARE | 19.3% curriculum; 18 of 43 real calls; contains read_file/read_document | 342 |
| Edit | DECLARE | 5.3%; 49 old real; `edits` covers multi-block edits | 613 |
| Write | DECLARE | 0.8%; 88 old real; new files | 289 |
| Find | DECLARE | 7.9%; the only navigation tool in real use (7 of 43) | 490 |
| Symbols | DECLARE (new) | 7.6% curriculum; 0 real calls because not declared; needs a short form | 2743 long, target about 500 short |
| Grep | DECLARE | 7.4%; exact patterns; 15 properties but 7 used (section 6.3) | 581 |
| TodoWrite | DECLARE | 20.2% curriculum; 12 of 43 real; the plan gate (S6) depends on it | 354 |
| AskUserQuestion | DECLARE | 0.7%; 14 desktop; safety valve; needed before destructive actions | 500 |
| WebSearch | DECLARE | 0.7%; 22 desktop; `read=3` makes WebFetch rarely needed | 302 |
| WebFetch | DECLARE | 0.8%; 23 desktop; the only way to open a URL the user gives | 303 |
| ToolSearch | DECLARE | 4.2%; the door to everything else; all 4 real calls searched for a project name, not a tool (section 1.3) | 301 |
| Glob | BY NAME in the prompt, FOLD into Find in v8 (opinion) | 3.0%; 1 real; 0.5% of old calls; Find answers names too | 277 |
| Skill | BY NAME (the environment line lists skills) | 0.8%; 1 old call; declared only when skills exist | 650 |
| TaskStop, Monitor | BY NAME | 0.2% each; Monitor = Bash `run_in_background` + Read | 433, 809 |
| NotebookEdit | BY NAME | 0.2%; niche | 1408 |
| EnterPlanMode, ExitPlanMode | BY NAME | 0.1 and 0.2%; the engine has the lock | 363, 457 |
| Agent, SendMessage, ListAgents, Workflow | BY NAME, **do not teach more** | 0 real calls; serial, re-prefilled sub-agents; 5 to 6 curriculum calls each | 627, 855, 459, 1089 |
| SubagentHandback | **REMOVE from the main agent's name list** (only the sub-agent's prompt) | 5 of 5 curriculum calls are errors by design: in the main agent it can only be wrong | 404 |
| EnterWorktree, ExitWorktree | BY NAME | 0.1%; asked for by the user only | 986, 842 |
| CronCreate/Delete/List, ScheduleWakeup | BY NAME; FOLD the four into one scheduling tool in a later version (opinion) | 22 curriculum calls total, 0 real; two overlapping ways to schedule | 1335, 300, 271, 965 |
| PushNotification, SendUserFile, EndConversation | BY NAME | 0.1% each | 618, 729, 568 |
| ListMcpResourcesTool, ReadMcpResourceTool, WaitForMcpServers | BY NAME | 0.1 to 0.2%; MCP tools themselves are 1.6% of the curriculum (`server__tool`) | 535, 513, 561 |
| LSP | BY NAME | 0.2%; Symbols covers the same questions with no server; keep for `hover`/`diagnostics` | 1424 |
| PowerShell | BY NAME, Windows only | 3 curriculum calls, all errors | 820 |
| TaskCreate/Update/List/Get | **FOLD into TodoWrite** or REMOVE (opinion) | 28 curriculum calls, 0 real; a second plan format; already out of the name line | 581, 768, 194, 293 |
| ReportFindings | BY NAME | 5 curriculum calls, 0 real | 1278 |
| Rename, Replace, Patch | **BY NAME, 0 examples: teach or remove in v8** | 0 curriculum, 0 real; `x-rename` teaches the manual path instead | 1580, 1812, 1095 |
| old run_command, read_file, write_file, edit_file | RETIRE (accepted, not named) | the table set covers all (section 4); 3 of them carry the old real history | - |
| old list_dir | RETIRE after giving `Bash ls` a home (Read of a folder) | 148 old calls, 100 repeats | - |
| old find_file | RETIRE only after machine-wide location exists in the new set | section 4.1, point 5 | - |
| old search_files, glob, web_search, web_fetch, read_document, ask_user, todo, job, delegate, terminal_screen | RETIRE | covered by Grep, Glob, WebSearch, WebFetch, Read, AskUserQuestion, TodoWrite, Bash/TaskStop, Agent; 0 curriculum | - |
| old terminal | BY NAME (keep) | only stateful shell; 73 old calls; taught 9 | - |
| old git, download_file, install_package, read_log, howto | BY NAME (keep) | taught 5 to 12 calls each; each has no table twin | - |
| old remember, recall | BY NAME (keep, or FOLD in one `Memory` tool later) | only durable memory; taught 7 and 5 | - |

**Counting the effect on the prompt for a base model:** the declared block goes from 12 short forms (4893 chars) to 12 short
forms with Symbols in and Glob out (about 4893 - 277 + 500 = 5116, **an increase of about 220 characters, about 55 tokens
(opinion)**); the "More tools exist" paragraph (805) loses `SubagentHandback`, the 4 Task* are already absent, and gains `Glob`.
For the trained model the prompt does not change at all, which is the point: the change is in what the curriculum teaches.

## 6. The proposed central list (12)

### 6.1 The list

`Bash, Read, Edit, Write, Find, Symbols, Grep, TodoWrite, AskUserQuestion, WebSearch, WebFetch, ToolSearch`

It is the present list (12 declared, 13 with `Skill`) with `Symbols` in and `Glob` out. The conditional `Skill` declaration goes
away: the environment line already lists the skills (`sk_env_line`) and v7 teaches `Skill` (26 calls).

**Why 12 and not fewer** (opinion, with the reasons): the research the project already collected says few simple tools win with small
models: SWE-agent's ACI (interface design changed the result; search that lists files only, capped at 50, beat both no search
and iterative search, 18.0% against 12.0% and 15.7% on SWE-bench Lite), LocAgent (three tools, fine-tuned 7B at 88.32% file
Acc@5 against 94.16% for a frontier model) and Anthropic's tool-design guidance ("too many or overlapping tools can distract
agents"). The BFCL page the project cites "did not test large tool sets" (`uioe-context-memory-routing.md` 6.1), so what it says
about 46 tools is an inference, not a measurement. RAG-MCP (13.62% to 43.13% when only relevant tools are shown) is the closest
evidence about many tools and is the reason to keep `ToolSearch` as a door instead of a longer list. CodexGraph's free-form
queries failed with a weak model (5.00 against 8.20 with no retrieval), the reason `Symbols` has a closed `action` vocabulary.
None of this is a measurement of katana; the project is not measuring in this phase. Below 12, the cuts would be: `WebFetch`
(folded into `WebSearch`, which already reads pages), or `AskUserQuestion` (by name); both are used (23 and 14 desktop calls), so
I keep them.

### 6.2 Navigation: Find and Symbols, one decision rule

Today the rule in `prompt.c` is a paragraph of 811 characters mixing five tools. The proposal is **one question**, asked of the
model in the rules text and the tool descriptions, with the same words:

> **Do you already know an exact name (a function, a type, a file)?**
> Yes: `Symbols` (what it is, where it is, who calls it, what it calls, what a change breaks, what a file contains).
> No, you only have words, a behaviour or an error message: `Find`.
> The exact text of a line or a pattern: `Grep`.

The three-line form, as the prompt text **(opinion on wording)**:

```
- Code: know the name? Symbols (action def, callers, callees, refs, uses, impact, outline, at, deps, flow).
  Only words about what it does, or a message? Find. An exact text or pattern? Grep. Then Read only the part it points to.
```

How the data supports it: the curriculum already separates them by what the user typed (`x-symbols`: 128 `Symbols` + 24 `Read`;
`x-find6`: 103 `Find` + 92 `Read` + 32 `Grep`; `x-find7`: `Find` 85, `Symbols` 8). The rule makes the boundary the model's input, not
the tool's internals.

**Tool-side backing so the rule does not depend on the model's judgement alone** (opinion; code changes for later):

- When a `Find` query is a single identifier that exists, `Find` already answers `[exact]`; append one line:
  `[katana: it is a defined name: Symbols callers|callees|impact answer from the code index]`. The same note pattern already
  works for the other `[katana: ...]` lines the curriculum teaches (family `x-katananotes`, 50 conversations).
- When `Symbols` gets a name it cannot find (`NOT FOUND`), it already tries other spellings; add a pointer to `Find` for words.
- `Find mode=refs` and `Symbols refs` return the same thing; keep both working. The trained model uses `Find refs` (68 calls); do not
  remove it before v8 teaches `Symbols refs` (0 calls today).

**Symbols: what each action needs to change** (all of this is for after v8; the current engine accepts everything below except `tree`):

| action | argument | state in v7 | change proposed |
|---|---|---|---|
| def | `name` | 27 calls | none |
| callers | `name` | 117 | none (the most taught action) |
| callees | `name` | 35 | none |
| impact | `name`, `depth` | 38; `depth` used 6 times | none; `depth` default 3 is enough, the argument can stay out of the short form |
| outline | `file` | 22 | none |
| at | `file`, `line` | 9 | needs more examples (the smallest of the used set); `line` was passed in 9 calls |
| deps | `file` | 5 | needs more examples |
| refs | `name` | **0** | teach, and make it the answer to "where is X used" so `Find mode=refs` can be demoted |
| uses | `name` | **0** | teach, or fold into `refs` with a `kind` filter (opinion); the long description needs 3 sentences to separate refs/uses |
| flow | `name` or `file`(+`line`) | **0** | new action; teach with `x-flow` situations: text keys between processes (WebSocket types, env vars, routes). It is the only action for "A talks to B without importing it" |
| tree | `path`, `depth` | does not exist; designed in `docs/code-index-lifecycle.md` J.5 | one new top-down action: answered from the cheapest index level at once, folders with sizes and one-line summaries; **the last of the 11 actions; do not add another after it** |

Argument surface: `Symbols` has 7 properties (`action`, `name`, `file`, `line`, `limit`, `depth`, `path`) with only `action` required.
`limit` was used **0 times** in 253 curriculum calls; `depth` 6; `line` 9; `path` 12. The argument that is always right is `name`
(217 of 253) or `file` (36). **Proposal (opinion): one `target` argument** ("a name, a file, or file:line"; the engine decides which, as
`sym_query_text` already does with `target`), so the model never has to pick between `name` and `file` for the action. That needs
retraining, so it is a v8 candidate, not a today change. Keep `limit` and `depth` accepted but out of the short form.

`Find`: keep `query` (always), `path` (21 calls), `mode` (68, all `refs`). **Proposal:** drop `limit` from the short form (never
sent by the model in the 263 curriculum calls); keep `mode` but let `Symbols refs` replace `mode=refs` in v8.

### 6.3 The other ten, what each needs

| tool | change | basis |
|---|---|---|
| Bash | none; keep `description` (the curriculum sends it in 444 of 444 calls) | any change breaks a perfect habit |
| Read | none to the arguments; **let `Read` of a folder return the listing** (names, sizes, dates) instead of an error | `list_dir` 148 old calls; `Read` says "It cannot list directories"; the old tool's job has no home |
| Edit | none; keep `edits` | curriculum 6 uses; `replace_all` 38 |
| Write | none | |
| Grep | the short form already has 7 properties of 15; keep. The 3436-char long form is paid once per fetch, never in the working prompt | `output_mode` in 218 of 248 calls |
| TodoWrite | **make `done` safe**: if there is no list, the error should say how to write the first list in the model's own words, and the curriculum has a family for it: "done before todos" was the only error of the 12 real calls | section 1.3 |
| AskUserQuestion | none | 23 calls, 0 errors |
| WebSearch / WebFetch | none | 25 and 28 calls |
| ToolSearch | **make a non-`select:` query useful**: the model sent `katana` and `select:katana` (4 calls) for a project name. A query that matches no tool could answer "No tool named katana. Tools by name: ..." with the 29 names, instead of `No matching tools` | 4 real calls, 4 for a project name |
| Glob | by name now; fold into `Find` as `mode=files` in v8 if the owner agrees (opinion) | 3.0% |

## 7. Migration of the old set

### 7.1 What still depends on it

| dependent | what | evidence |
|---|---|---|
| dispatcher | accepts the names with `strcmp`; the table is tried first (`agent_tool_run`) | `dispatch.c:28-52` |
| prompt (base models) | `KATANA_TOOLS=1` declares the old set; default is the table (`ct_set2`) | `prompt.c:139` |
| prompt (all models) | the "Also:" line names git, terminal, recall, remember, download_file, install_package, read_log, howto | `prompt.c`, the `More tools exist` paragraph |
| curriculum generator | sets `KATANA_TOOLS=2` (the table) while generating | `curriculum.c:2531, 2593` |
| `howto`, `webnav`, `loop` | texts that change by `ct_set2()` (`TodoWrite` or `todo`, `Read` or `read_document`) | `howto.c:112`, `webnav.c:160, 192`, `loop.c:439, 441, 513, 854, 909` |
| call grammar | `callgrammar.c:651`: "the table, even with KATANA_TOOLS=1" | the grammar is already table-only |
| desktop | `Verbs.js` holds the verbs of the old names; `backend.cpp` treats `todo` as progress, `ask_user` as a CHOICE marker (`backend.cpp:1707`, `ask.c`), `edit_file` in a comment | `desktop-qt/qml/Verbs.js`, `backend.cpp:1829` |
| saved sessions | `.ksess` and `.json` of 2026-11-20 and 2026-11-21 (old set: 183 `run_command`, 130 `edit_file`, 46 `todo`) replay by name | desktop sessions |
| tests | `tools/tooltest/*.sh` use old names in 3 to 7 scripts each; `tools/testar_ferramentas.sh` | `grep -lw` counts: `run_command` 7 scripts, `read_file` 7, `write_file` 7 |
| MCP server | `katana mcp serve` exposes only Read, Glob, Grep, Write, Edit, NotebookEdit, Bash (`mcp_serve.c:13-21`): **no Find, no Symbols, no LSP, no web**; the old set is not exposed | audit: "katana mcp serve does not expose Find/Symbols/LSP" |
| `brief_guard` | runs only in the table dispatch (`table.c`, `ct_dispatch`), so an old-name call bypasses the `never:`/`ask:` guards of `KATANA.md` | audit weak point 4 |

The last row is the real cost of keeping the old set: **an old-name call skips `brief_guard`** (the project rules the owner wrote in
`KATANA.md`). That is the strongest reason to stop accepting the old mutating names (`run_command`, `write_file`, `edit_file`) first.

### 7.2 Order

1. **Now, no retraining:** route the old *mutating* names through the table's guards: `run_command` -> `Bash`, `write_file` -> `Write`,
   `edit_file` -> `Edit` with key translation (`command`, `path` -> `file_path`, `old` -> `old_string`, `new` -> `new_string`;
   `append` and `sudo` have no table equivalent, keep their old code for those two). This closes the `brief_guard` hole without
   changing what any model sees. (code change; not made here)
2. **Close the two real gaps before retiring anything:** machine-wide location (`find_file`) and a directory listing (`list_dir`),
   as `Find`/`Read` behaviour (section 6.3), not as new tools.
3. **Stop naming:** the 16 old names that v7 does not teach are not named anywhere in the working prompt already; the "Also:" line keeps
   the 8 taught ones. Nothing to do.
4. **Desktop:** keep `Verbs.js` entries (cheap, and old sessions must still render) and keep the `todo` progress case; **add `Find` and `Symbols`
   to the verbs**: `Verbs.js` has no entry for either (checked with grep; the card shows the raw tool name, `verbo(n) = verbos[n] || n`),
   although they are 7.9% and 7.6% of what the model is taught.
5. **Tests:** move `tools/tooltest` scripts to table names; the old names stay in a single compatibility script.
6. **After v8 is the only model in use:** drop `KATANA_TOOLS=1` and the old declaration branch of `agent_build_sys` (about 40 lines of
   `TOOLDEF`); keep the dispatcher's accept-list for saved sessions only.

### 7.3 `katana mcp serve`

Not part of the working prompt, but part of the tool set a user sees: it exports 7 tools with `readOnly`/`destructive`/`idempotent`/
`openWorld` annotations (`McpsTool`). Adding `Find` and `Symbols` there costs one table row each plus the roots plumbing (the file
already has `MCPS_MAXROOT`); both are read-only. **(opinion)** If the project's pitch is "a code index other agents can use", these
two are the reason to run the server, and today it does not offer them.

## 8. What the v8 curriculum would have to teach (list only)

This is the list, not data; nothing was generated. It follows `docs/plan.md` T5 (teach `Symbols flow`, `Symbols tree`, the context
block, the index state lines) and adds what this review found.

**Teach (new or more):**

1. `Symbols`: `refs` (0 today), `uses` (0), `flow` (0), `tree` (new, when built), more `at` (9) and `deps` (5). v7 has 253 `Symbols` calls
   in 183 conversations; the three missing actions are 30% of the vocabulary.
2. The one decision rule of section 6.2, as **paired situations**: the same repository, one request with a known name (`Symbols`),
   one with words (`Find`), one with an exact text (`Grep`). v7 has the families but not the contrast.
3. `Find` as the answer to "where is X" when the name does not exist (`NOT FOUND` then words), and `Symbols` after `Find` hits an
   identifier (`x-find7` has 8 such `Symbols` calls only).
4. `TodoWrite`: the first call always has `todos`; a conversation where `done` is sent first, receives the error, and recovers
   (the one real failure).
5. `ToolSearch`: `select:` of a **tool** name only, and a case where the model wants a project name and does not use `ToolSearch`
   (all 4 real calls were for a project name).
6. Wrong-path recovery: a request with a folder the user names loosely, the `[katana: you mention ...]` note, and `Find`/`Glob` with `path` (the
   2026-11-23 session started from a wrong path; the note exists since that day).
7. If `Glob` is folded: `Find mode=files` (or `pattern`) replaces the 99 `Glob` calls.
8. If `Read` lists a folder: examples where the argument is a folder.

**Decide before generating** (owner):

- `Rename`/`Replace`/`Patch`: teach (change `x-rename` to call `Rename` with `dry_run`) or remove. Today the family trains the manual
  path and the tools stay unknown.
- `Task*`: keep as by-name or remove; 28 calls, same job as `TodoWrite`.
- Old-set names (8 taught): fold `terminal`, `git`, `download_file`, `install_package`, `read_log`, `howto`, `remember`, `recall` into
  table-style names (`Terminal`, `Git`?) or keep. Moving them costs retraining; leaving them costs a second naming style.

**Drop (reduce, not teach):**

- `SubagentHandback` from the main agent's examples (5 error examples in 5; it belongs to the sub-agent prompt).
- Fewer examples for the 29 by-name tools: `x-rare` is 152 conversations (14.6% of the file) and 116 `ToolSearch` calls for tools with 0
  real uses. A smaller `x-rare` frees room for Symbols/Find contrast. **(opinion)** The "rare" family is what makes `ToolSearch` 4.2%.

**Keep as is:** TodoWrite 677, Read 648, Bash 444, Edit 177, Grep 248, Find 263, WebSearch/WebFetch 25/28, AskUserQuestion 23.

## 9. Risks and what NOT to change without retraining

**Do not change without retraining the model** (the names and keys are in the model's weights, not in the prompt, because of
`--no-tool-decl`):

- The **name** of any tool in the curriculum's 14 big tools, and of the 8 taught old-set tools.
- The **argument keys**: Read `file_path`, `offset`, `limit`; Edit `file_path`, `old_string`, `new_string`, `replace_all`, `edits`; Write
  `file_path`, `content`; Bash `command`, `description`, `run_in_background`; Grep `pattern`, `path`, `glob`, `output_mode`, `-i`, `-n`,
  `-A`; Glob `pattern`, `path`; Find `query`, `path`, `mode`; Symbols `action`, `name`, `file`, `line`, `depth`, `path`; TodoWrite
  `todos`, `done`; ToolSearch `query` (`select:`).
- The **values**: `Symbols` action names, `Find` mode `refs`, Grep `output_mode` (`files_with_matches`, `content`, `count`).
- The **result formats the curriculum shows**: TodoWrite's "Step N of M: ..." line, `Read`'s `line<TAB>text`, the `[katana: ...]` line
  prefixes, `NOT FOUND`, `[exact]`/`[probable]`/`[confidence ...]`. The curriculum is replayed against the real tools
  (`katana curriculum check`); a change in output wording changes what the model sees next to its own past examples.
- The **order and content of the prompt text**: the rules text is part of every training conversation (system turn of about 1936
  characters on average, `sys chars` of the 1042 conversations) and the prefix cache keys on it.

**Safe without retraining** (additive or behind the tool):

- Accepting more argument spellings (aliases), more output after the existing lines, new `[katana: ...]` notes the model has seen a
  family of (`x-katananotes`).
- Routing old names through the table's guards (section 7.2 step 1).
- Adding `Read` of a folder, a pointer in `ToolSearch`'s empty answer, a note in `Find` results (the model has to ignore unknown notes,
  and it has been trained to follow `[katana: ...]` lines, so a *new kind* of note is a risk, a repeated kind is not).
- The declaration block for base models (`g_ct_short`): it does not exist for the working model.

**Risks of the proposed list** (opinion unless a number is given):

1. **Symbols in the list does nothing for the working model by itself.** The model learned it from 183 conversations and used it 0
   times in 43 real calls; the declaration block is absent from its prompt. What would change real use is more `Symbols` situations
   in the real tasks, which v8 must make explicit; no declaration change replaces that.
2. **A model that uses `Find` for everything is fine and cheaper:** 7 real `Find` calls and no failure. If `Symbols` stays at 0 in
   real sessions after v8, the honest conclusion is that `Find refs` plus `Read` covers the need and the 10 actions are
   index work without users.
3. **Folding `Glob` loses a deterministic, index-free tool**; if the index is stale or the folder is not indexed (home folder, ignored
   folders: the case the audit and `code-index-lifecycle.md` describe), `Glob` still works. Keep it accepted and by name.
4. **Retiring `find_file` before machine-wide location exists** brings back the failure of 2026-11-23 (a folder the model could not locate).
5. **The Windows build** has no `Find`, `Symbols`, `LSP`, terminal or jobs (`#ifndef _WIN32` in the audit's weak point 5): a central list
   with two code tools has no code tool on Windows. The list must stay per platform.
6. **Counting error**: sets A and the desktop sessions overlap (the same four sessions), so they are not independent samples; the
   43 calls are 4 sessions about one project. Any rate computed from them has a very wide margin. The curriculum numbers are exact
   but describe what the owner taught, not what users ask.
7. **TodoWrite's `done` shortcut** is 436 of its 677 curriculum uses, and the single failure mode of the real calls; it should be
   safe on the engine side before anything else (section 6.3).

## 10. Open questions for the owner

1. Glob: keep declared (and drop `AskUserQuestion` or `WebFetch` from the 12) or demote? Evidence is 1 real call; the cost is 277 characters.
2. `Rename`/`Replace`/`Patch`: teach or remove?
3. `Task*`: remove or keep by name?
4. Old-set names taught by v7 (terminal, git, ...): rename to table-style in v8 or keep?
5. Is the old mutating set to be routed through `brief_guard` now (section 7.2 step 1)? It is the only item with a safety argument.
6. Should `katana mcp serve` offer `Find` and `Symbols`?
7. A fresh batch of real sessions (the current evidence is 4 sessions, 43 calls) before deciding the list; the log keeps only 400
   characters of arguments, so a lossless decision log (audit M4) would make the next review exact.
