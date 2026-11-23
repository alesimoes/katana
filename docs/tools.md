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
| `glob.c`, `grep.c` | `Grep` (`glob.c` keeps only the pattern matcher and the folder walker that `Grep` and the UIOE file connector use; the `Glob` tool was removed) | - |
| `regex.c` | the regular-expression engine used by `Grep` | 415 |
| `symbols.c` | `Symbols` (def, refs, callers, callees, uses, outline, at, deps, impact, flow, tree over the symbol index; work in `src/coder/symtool.c`) | - |
| `find.c`, `findgraph.c` | `Find` (navigation: index, search, references; added 2026-11-22) and its file graph, sparse activation and neighbour note | 59 |
| `webfetch.c`, `websearch.c` | `WebFetch`, `WebSearch` | 45 |
| `tasks.c`, `askuserquestion.c`, `planmode.c` | `TodoWrite`, `AskUserQuestion`, `EnterPlanMode`, `ExitPlanMode` | 80 |
| `agent.c`, `messages.c`, `workflow.c` | `Agent`, `SendMessage`, `ListAgents`, `SubagentHandback`, `Workflow` | 42 |
| `mcpres.c`, `toolsearch.c`, `skill.c`, `reportfindings.c` | `ListMcpResourcesTool`, `ReadMcpResourceTool`, `WaitForMcpServers`, `ToolSearch`, `Skill`, `ReportFindings` | 71 |
| `worktree.c`, `powershell.c`, `notify.c` | `EnterWorktree`, `ExitWorktree`, `PowerShell`, `PushNotification`, `SendUserFile`, `EndConversation` | 61 |
| `lsp.c` | `LSP` | 39 |
| `schedule.c` | `CronCreate`, `CronDelete`, `CronList`, `ScheduleWakeup` | 156 |

Measured: the 1244 checks pass; `tools/traintest/run.sh` passes, including a whole-loop run with the tiny test model
and the mock teacher with this set (Write, Edit, Bash with the working-directory reset, Grep).

The prompt declares 12 tools (the lean list of `docs/tools-review.md`): `Bash`, `Read`, `Edit`, `Write`, `Find`, `Symbols`,
`Grep`, `TodoWrite`, `AskUserQuestion`, `WebSearch`, `WebFetch` and `ToolSearch`, and lists the rest by name
(`Skill` and `SubagentHandback` are not listed: skills are named in the environment block, the hand-back exists only inside a
sub-agent). Removed from the table, with no alias (2026-11-23): `Glob`, `Rename`, `Replace`, `Patch`, `TaskCreate`,
`TaskUpdate`, `TaskList`, `TaskGet`; file names are found with `Find` or `Bash ls`, changes in many places are `Grep` plus
`Edit` (`replace_all`) or a `Bash` script, and the plan is `TodoWrite`. The table has 37 tools. The teacher
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

**Map of a large file (2026-11-22).** `Read(file_path)` with neither `offset` nor `limit` on a source or text file of a known
extension (c h cpp hpp cc js mjs cjs ts tsx py kt java go rs sh md qml ps1) that has more than 200 lines or 8 KB does not return
the whole file. It returns a header line (`[path: N lines, B bytes]`), a map of at most 60 symbols with their line numbers in
file order (functions, classes, structs and enums, markdown headings, shell functions and `# ---` section headers, the first 12
top-level `#define`s; when there are more than 60 an even sample is shown), the first 40 lines numbered, and the note
`[katana: file has N lines; this is its map and its start. Read a part with offset and limit (40 to 80 lines), or search inside it with Grep (-n -C 2).]`.
The matchers are simple hand-written line patterns (`ct_outline` in `src/tools/read.c`), a guide for where to read, not a parser.
Any `offset` or `limit` gives the old behaviour; files below the threshold, other extensions, PDFs and notebooks are unchanged;
`katana tool Read` uses the map too. The file counts as partly read: `Edit` needs a whole read, so for a large file read it
with `offset=1` and a `limit` as large as its line count first (the `Edit` error says so). If the file was already read whole
in the conversation, the unchanged/changed-part notes of the delta reading (`facts.c`) come first. Switches:
`KATANA_READ_OUTLINE=0` off, `KATANA_READ_OUTLINE_LINES=N` changes the line threshold (the byte threshold becomes 40 x N).

**Repeated identical calls.** In the agent loop (`rr_check` in `src/agent/loop.c`), when the model repeats, in the same request,
a `Read`, `Grep`, `Glob`, `Find`, `Symbols`, `WebSearch`, `WebFetch` or MCP tool marked `readOnlyHint` (server of the user's own configuration) with the same arguments as a
call that already succeeded, the call is not run again: the result is the note `[katana: you already made this exact call at step N
of this request; its result is above in this conversation, or listed there as removed to save context. Do not repeat it: pick a
different place to look ...]`. From the
third repetition on the note is firmer and lists what the model can still do (another part with offset and limit, `Grep` with
`output_mode` content, `-n -C 2`, `Glob` for the defining file, or answer saying what is unknown). `Bash`, `Write`, `Edit` and
the other tools are not covered; any call to one of them that is not a read clears the table, because the same read may then
give another result (the delta notes take over). Failed calls are not recorded (the failure-repetition rules handle them). A new
user request starts over. `KATANA_REPEAT_NOTE=0` turns it off. Test: `tools/tooltest/loopnotes.sh` (`katana toolrun notes`).

### Grep
(The `Glob` tool was removed on 2026-11-23; its pattern matcher, `ct_gmatch`, stays for the `glob` argument of `Grep`.)
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
Nested roots: searching from a folder does not descend into a subfolder that has its own `katana init` (marker
`.katana/coder/ROOT`) or is a linked git worktree (`.git` is a file pointing into `worktrees/`), the same decision the index
makes (`rt_skip_subdir`, `src/coder/roots.c`); this holds in both file lists (git and `.gitignore` walk). Searching with
`path` set to that nested folder (or from inside it) works normally, since it is then the root of the search.

### Find (navigation)
`Find(query, path, limit, mode)` (`src/tools/find.c`): the navigation tool. The model says what it is looking for, in
plain words and/or identifiers, and gets a few precise candidates instead of whole files. Read-only (runs in plan mode).
- `mode=search` (default): up to `limit` candidates (default 6, max 12), best first. Each one is
  `file:firstLine-lastLine` (a 40-line chunk), the symbols defined in that chunk (`defines:`; `sections:` for
  Markdown headings), the 1 to 3 lines of the chunk with the most query terms, `calls:` (up to 4 names used in the
  chunk that are defined elsewhere, with `file:line` of the definition: the outgoing edges) and `also referenced in:`
  (other files that use the symbols defined on the shown lines: the incoming edges). The answer stays under about
  1500 characters (`KATANA_FIND_CHARS`); candidates that do not fit are counted at the end. It ends with
  `Follow a symbol with Find mode=refs, or Read a part with offset and limit.`
- `mode=refs` with a symbol name in `query`: where it is defined (exact case first, then any case) and the lines
  that use it, grouped by file (definition file first, documentation last), each use with the function it is in
  (`301 (in onMessage): ...`), up to `limit` files and 3 lines per file.
- `path`: a subfolder or one file of the working folder (filters the results); a folder outside the working folder
  gets its own index.

**Unified index (default since phase 7 of `docs/code-index-lifecycle.md`; `KATANA_FIND_INDEX=store`).** `Find` no longer keeps an
index of its own. It reads the coder store (`<root>/.katana/coder/store` or the home index, see `katana coder status`) and the files
themselves; no `find-*.idx` is created, read or updated, and `Find` makes no walk of the tree (the only update is the shared
`index_ensure`, which creates the coder index on the first call in a folder and keeps it current, as `Symbols` and the coder layer
already did). Per call (`src/coder/findstore.c`):
1. *Candidates.* The query is tokenised as always; for each term the lexicon of the store gives the files that have it (normalised
   and original spelling keys, search fields path, name, word, part). A file-level BM25F over those postings (path and name weigh 3,
   word 1, part 0.5; coverage bonus; the same documentation, config and test factors as the ranking) picks the best 100 files
   (`KATANA_FIND_CAND`, 10 to 300). Added to them: the files the coder name and text engine suggested for the question (`CoderHits`, up to 12) and
   the files that define a name equal to a query term (symbol index, so a common word such as `list` still reaches the file that defines `list`).
2. *Mini-index.* Only those files are read from disk, cut into 40-line chunks and tokenised with the same code as before (path, symbol
   names, body fields); the usual ranking runs over them. The idf uses estimates of the whole project: document frequency of the term in the
   store lexicon (files) times the chunks per file seen in the candidates; the term table is kept between calls (so the per-request state
   keeps working) and the chunk postings of recently read files (300 files, keyed by path, mtime and size) are kept in the process, so a second
   question about the same files neither reads nor tokenises them again.
3. *Graph.* The file graph is a section of the store (`STS_GRAPH`, `src/coder/graph.c`) derived from the levels already indexed, not from
   the files: `sym` edges join the references and the definitions of the symbol index (and, for the same defined names, the files whose text
   has the name, from the lexicon, because comments, strings and properties are not references); `path` edges are the imports resolved to files
   (N3) plus files that name another file with its extension (lexicon again); `rare` edges join files that share a name that is referenced in 2 to 4
   files and defined nowhere, or a text key (event, route, environment variable, config key) found in 2 to 4 files; `co` edges come from `git log`
   as before (`KATANA_FIND_CO=0` skips them). The same weights, row normalisation, sparse activation and neighbour note as the old graph read it.
   The crawler writes it when its queues are empty (`graph_on_quiet`); a `Find` on a folder whose graph is missing or no longer matches
   the documents (one signature over content hashes, so compaction does not invalidate it and an edit does) derives it in the same call, up to
   `KATANA_FIND_GRAPH_MAX` files (default 30000; above that an older graph is used as it is), and saves it. `KATANA_FIND_GRAPH=0` stops the
   crawler and `Find` from deriving it. Neighbours that enter the answer through an edge (and the file they came from) are read and added to the
   mini-index for the chunk, the symbols and the `why:` line. The neighbour note in the agent loop reads the graph from the store and never
   creates an index or a graph.
`calls:`, `called by:`, `links:` and `next:` still come from the symbol index; `also referenced in:` now lists files with real references to the
symbols defined on the shown lines (documents of plain text are not listed: they have no references). `mode=refs` is answered by the symbol index
and, when it does not know the name, by the text path over the files that contain it (up to 300, from the lexicon). A first call in a folder without an
index creates it (a few seconds for a large folder) and answers. If the coder index cannot be created or read the call falls back to the old
index of its own. `KATANA_FIND_TIME=1` prints the times (index, candidates, mini-index, graph and where it came from).
`KATANA_FIND_DEBUG=1` also prints the candidate scores and the activation.
Memory: nothing of the project stays in the heap except the term table, the 300 recent files and the graph rows of the call (the segments are mapped
and shared with the crawler and other sessions). The output format (lines, budget, request state, dedupe, next page) is unchanged.
Known difference from the old ranking: only the candidates enter the chunk-level ranking, so a file that does not reach the candidates is never scored
(`KATANA_FIND_CAND` raises the number); the idf is an estimate; the graph has fewer text-only edges for languages without import resolution.
`Rename`, `Replace` and `Patch` still use the old index of their own (`fd_update` in `src/tools/multiedit.c`), which creates `find-*.idx`.

**Old index (`KATANA_FIND_INDEX=legacy`, and the fallback).** Files: `git ls-files --cached --others --exclude-standard` inside a repository, otherwise a walk that
applies `.gitignore` (the same listing as `Grep`); then skips `.git`, `node_modules`, `build`, `dist`, `vendor`,
`.katana`, `target`, `.venv` and similar folders, files above 300 KB, binary files (a NUL byte in the first 8 KB) and
extensions that are not source or text (c h cpp hpp cc glsl comp js mjs ts tsx py kt java go rs sh md qml ps1 xml
toml yml yaml cfg txt cmake gradle swift m, a few more). At most `KATANA_FIND_MAX_FILES` files (default 20000; the
answer says when files were left out). Each file becomes 40-line chunks with stride 25, three fields per chunk: path,
names of the symbols defined there, body. Terms: accents folded, each identifier kept whole (`listSessions` ->
`list_sessions`) and split (camelCase, snake_case), lower case, stop words removed, simple stemming. Symbols come from
per-language line rules (functions, classes, structs, `#define`, typedefs, globals, JS `const x = () =>`, methods,
Python/Go/Rust/Kotlin/Java definitions, shell functions, QML `id:`/`property`/`signal`, PowerShell functions,
Markdown headings). The reference graph is the inverted index itself: the whole-identifier term leads to every chunk
that names it; one-part names are confirmed word by word in the file text.
Stored in `<working folder>/.katana/cache/find-<hash>.idx`, the same folder as the prefix cache, with a `.gitignore`
containing `*` (in `~/.katana/cache` when the working folder is the home folder or not writable; `KATANA_CACHE_DIR`
overrides). Every call lists the files again and re-reads only those whose mtime or size changed; files that
disappeared leave the index; a damaged file is rebuilt. Inside one agent process the index stays in memory.

**One entry point for search (decided by katana, not by the model).** The model has one rule: `Find` first, `Grep`
only for an exact pattern, `Glob` only for file names by pattern. `Find` looks at the question and picks the channel:

| Question | Channel | What is added to the usual answer |
|---|---|---|
| text in double quotes (3+ characters) | literal check by the coder engine, files are re-read | `Literal text found:` with `[confidence high] file:line: text`, or `NOT FOUND (literal): ...` |
| one identifier or file name (has `_`, `.`, `/` or camelCase) | exact lookup by the coder engine | `Found by name or text (checked in the files):` with the first line that has the text, or the file named like that; with none, `NOT FOUND: nothing is named "x"` and up to 2 `[probable]` similar names or spellings |
| a description in words | the usual `Find` (BM25 + file graph); with `KATANA_CODER=full` the coder engine (exact, sub-word, BM25, trigram, literal and conceptual channels) is a second layer | with `full`: `Also from the text search:` up to 3 files the graph did not show (only `[exact]`, or `[probable]` with a concept+operation pair in the path) and `NOT FOUND in the index: ...` |

The usual candidates (file:range, `defines:`, `calls:`, `also referenced in:`) always come out whole after the added
lines; the coder lines are a few short lines, the file graph is never replaced. Coder results are kept only for files
that the `Find` index lists too (the coder index does not read `.gitignore`). `[confidence high]` means the text was found in
the file (or the file has that name); `[confidence low]` means a similar spelling or a weaker channel. There are no fixed grades: a lookup tries the exact
name first, then derived matches, then probable ones, and each result carries its confidence level. When `Find` finds
nothing, the message ends with a suggestion to try `recall(query)`.

`KATANA_CODER`: `off` = `Find` exactly as before (no coder index is created); `names` (alias `exact`) = the coder engine
only for a quoted text or an identifier/file name, without the conceptual channel; `full` (default) = also for descriptions in
words, with the conceptual channel (knowledge base).

The coder index lives in `<working folder>/.katana/coder/store` (ignored by git; `~/.katana/coder/...` when the folder
is not writable), the same store as `katana coder index`. It is created by the first query that needs it and updated
incrementally on every such query (files with a new mtime or size are read again). `KATANA_FIND_TIME=1` prints the time.
`katana coder ...` keeps working unchanged (`katana coder search` and the `Find` layer run the same engine, `cs_search_core`
in `src/coder/search.c`, which `coder_lookup` calls without printing).

*Memory.* `Find` does not search saved facts: `recall` keeps its own tool (it answers about what the user or the
model saved, not about files), and a `Find` with no result tells the model to try it, in one line. Mixing the two would
return facts as if they were code locations.

**Guiding an investigation (Find as a whole).** After the BM25 score, each file score is multiplied by adjustments whose weights
are constants at the top of `src/tools/find.c` (`FD_W_*`): README and other `.md` x0.45 and config/text files x0.72 unless the
question talks about documentation, install or usage; test files (`test-*.mjs`, `*_test.*`, `tests/`) x0.55 unless the question
talks about tests; a bonus for the idf-weighted share of question terms found in symbols DEFINED in the file (`sessionId`
matches `session`), in the path, and anywhere in the file (a rare term such as the product name weighs more); a bonus for
files in the same folder as something already shown or read in this request; a bonus for the top-level folder that holds most
of the score of the best 12 files. `KATANA_FIND_DEBUG=1` prints the scores before and after. Per request (reset with the read
cursor) `Find` remembers the ranges it pointed to and the ranges `Read` delivered: a candidate whose chunk is at least half seen
is left out and listed in one `(already shown, skipped)` line, and the next candidate takes its place; a second `Find` with
(almost) the same words says so and returns the next page. The end of the answer has `next: file:lines (calls X)` /
`(callee X)`: callers and callees (from the coder symbol index) of the symbols that match the question, in ranges not yet
read (`KATANA_FIND_NEXT=0` turns it off); the "linked files you have not read yet" note skips files already named there.
For each of the first three results that is code (and when the folder has a coder index), the old `calls:` line of names is replaced by
three short lines about the symbol that contains the best line: `called by: connect (grey-channel.mjs:55-78), main (...)` (callers, same
file or not, by confidence, up to 3, `(+N)`; `nothing indexed (entry point or called by name from outside)` when none), `calls: toBridge
(grey-channel.mjs:53), ...` and `links: sends "register" -> handled in channels.mjs:33` / `handles "x" <- sent from a.mjs:61` (text-key links,
see `docs/code-intelligence.md` 12.8; at most two lines, closest to the best line first). Callers/callees of a file-private symbol in
another file or of another language family are demoted, and low-confidence ones are left out when better exist. Without an index the old `calls:` line stays.
`mode=refs` with a phrase (two or more words) runs a normal search with a one-line explanation; a single name with no
definition suggests similar names from the symbol index (case and underscores, part of an identifier, edit distance).
Output target is about 3000 characters (`FD_OUT_CHARS`, `KATANA_FIND_CHARS`).

**Ranking.** BM25 per field (weights path 3, symbols 1, body 1; k1 1.2, b 0.75), a coverage bonus of 10% per distinct
query term present in the chunk's symbols or body, file score = best chunk + 0.3 x second chunk. This is the best
algorithm of the research bench (`tools/findbench/bench.py`: weights 3/2/1, bonus 5%) ported to C, with the symbol
weight and the bonus retuned by measurement. `KATANA_FIND_TUNE="wp=3,ws=1,wb=1,k1=1.2,
b=0.75,cover=0.1,agg=0.3,idw=1,symb=0,docpen=1"` changes the parameters for research (`idw`: weight of query terms
written as identifiers; `symb`: bonus for a chunk defining a symbol named in the query; `docpen`: factor for
documentation files when the query has identifiers). `KATANA_FIND_TIME=1` prints the times to stderr.

**Measured (2026-11-22, this machine).** Questions are commit messages, the answer is the set of files of the
commit (`bench.py REPO --n 100 --stem --uniq`; `--katana ./katana` runs the C tool with the same questions; `--uniq`
drops repeated messages, which made hit@1 swing by 15 points on one repository).

| Repository (indexed files) | Python hit@1 / @5 / @10, MRR | C `Find` hit@1 / @5 / @10, MRR |
|---|---|---|
| test repository, C++/Python, seed 1 (5349) | 62 / 81 / 90, 0.711 | 64 / 89 / 93, 0.744 |
| same, seed 2 | 66 / 91 / 97, 0.760 | 67 / 95 / 97, 0.787 |
| katana, seed 1 (991) | 53 / 76 / 84, 0.626 | 57 / 83 / 86, 0.674 |
| katana, seed 2 | 55 / 77 / 87, 0.651 | 61 / 82 / 89, 0.705 |
| greymod (314; only 83 questions) | 65 / 93 / 94, 0.762 | 66 / 94 / 94, 0.769 |

Times (C, whole `katana tool Find` process): cold index of the 5349-file test repository 1.15 s (Python bench: 18 s),
katana 0.29 s, greymod 0.08 s; a call that reuses the index 0.14 s on the large repository (load 19 ms, file listing
and stat 102 ms, invert 18 ms), 0.02 s on katana; the query itself 1 to 7 ms. Index file 18.5 MB (large repository),
5.3 MB (katana); peak memory of the process 55 MB. Tuning (k1, b, field weights, aggregation, coverage, identifier weight,
symbol bonus, documentation penalty) moved hit@5 by at most 2-3 points either way on 100 questions, within noise;
the one change kept (symbol weight 2 -> 1, coverage 5% -> 10%) was equal or better in hit@5 and MRR on all five
rows above against the ported Python parameters (e.g. test repository seed 1: hit@5 82 -> 89). Symbol bonus 0.3 and
identifier weight 1.8 made results worse; the documentation penalty did nothing measurable.

The multi-hop case (the cause is in files that do not contain the words of the question) is the reason for `calls:`
and `mode=refs`. `tools/tooltest/find.sh` replays it on the greymod tree before the fix (commit 03ee8cf, question
"why does the gkatana command not show on the phone?"): `Find "gkatana celular Grey"` -> `linux/install.sh` (gkatana
uses the same channel as the older terminal command); `Find "sessoes de terminal listadas no celular" path=bridge` -> `bridge/server.mjs:326-365`
with `calls: listSessions (bridge/sessions.mjs:132), terminalSessionIds (line 114), approve (bridge/channels.mjs:106)`
(the three files of the fix in one answer); `Find terminalSessionIds mode=refs`
-> used in `onMessage`; `Find listSessions mode=refs` -> defined in `bridge/sessions.mjs:132`, used in
`bridge/server.mjs`; `Find createChannels mode=refs` -> `bridge/channels.mjs`. No whole file is read. The tree is
a detached worktree in `GREY_BEFORE` (default `/tmp/katana-grey-before`; the test skips this part when it is missing).

Not verified: whether Qwen3.6 actually calls `Find` and follows `calls:` / `mode=refs` instead of reading whole files
(no run with the model yet). `Find` is in the core list of the system prompt (`src/agent/prompt.c`, declared with its full schema, with a short guide
to which file tool to use when); the prompt grew by about 850 bytes. Windows: not available yet.

**File graph, sparse activation and the neighbour note** (`src/tools/findgraph.c`, ported from the research in
`docs/investigation-engine.md`; no language model was run to build or check it).
- *Graph.* (Old index; in the default mode the same graph is derived from the store, see "Unified index" above.) Nodes are the indexed files. Four undirected edge types, stored as one CSR (int32 destination, float32
  weight, 1 byte type mask) in the same cache file as the index (`FD_VERSION` 4: an older file is rebuilt without error;
  the graph section follows the file records). It is built lazily, the first time a search or the neighbour note needs
  it, and rebuilt whole (all files re-read once, plus one `git log`) whenever the index changed.
  - `sym` (weight idf): an identifier used in file A and defined, by the index's symbol rules, in file B. Only code files
    (not Markdown or config); names defined in more than 3 files or used in more than max(30, 2% of files) files are skipped,
    so there are no hubs.
  - `path`: file A names file B: a path-like string ending in a known extension (matched by file name, at most 3 files
    with that name), a relative import (`'./x'`, `'../y/z'`, tried as stem, as given, and as `/index`), or a Python
    `import`/`from ... import`.
  - `rare` (weight idf / (df-1)): an identifier-like token (underscore or camelCase, 6+ letters) found in 2 to 4 files that
    is not a defined symbol.
  - `co`: files changed together. Once per rebuild, `git log --no-merges --relative --name-only -n 2000` through
    `popen`; commits with more than 40 files are ignored; weight 1/(n-1) per pair, summed and capped at 3. No git, not
    a repository, or `KATANA_FIND_CO=0`: skipped silently.
  Each type is row-normalised, scaled by beta (sym 1, path 1, rare 1, co 0.5), then each row is normalised to 1.
  Measured size on the katana tree: 1003 files, 3389 sym + 1614 path + 1395 rare + 6629 co edges (258 commits), 20646 CSR
  entries, 0.2 MB, 177 ms to build. The specification predicted about 3 MB for the 4.7k-file repository (not rebuilt here).
- *Ranking of `Find search`.* Per-field BM25 as before, then: request words are dropped from the query (a short built-in
  Portuguese and English list: find, explain, why, where, project, code, file, descubra, onde, projeto, arquivo, nada...;
  if every term would be dropped, none is); the best 40 files (normalised by the best, below 0.15 dropped) become the
  active set; 2 rounds of spreading, the best 20 active files pushing `0.5 x activation x edge weight` to their neighbours,
  keeping only the best 20 new nodes (lateral inhibition); the files with activation of at least 0.15 x the maximum, filtered
  by `path`, are returned best first (default 6, max 12). No abstention and no automatic seed: the model always asks.
  A candidate that entered through an edge (and not through a text match) gets a line `   why: via <file> (symbol X)`,
  `cites the path of <file>`, `its path is cited by <file>`, `shares identifier X with <file>` or `changes together with
  <file>`, and its chunk is the one holding the symbol when known. Output format and the 1500-character budget are unchanged
  otherwise. `KATANA_FIND_ACT=0` restores the previous ranking (no request-word filter, no graph).
- *Neighbour note.* After each successful `Read`, `Grep` or `Find` result in the agent loop (`loop.c`, just before the plan
  note), `[katana: linked files you have not read yet: a (why), b (why), c (why)]` is appended: at most 3 graph neighbours
  of the file read (Read) or of the first 3 files in the result (Grep, Find), excluding the files shown in the result, files
  `Read` earlier in this request and files already named in an earlier note, only for neighbours whose summed edge weight
  is at least 0.06. No note after a repeated call, an error, or a short re-read (limit <= 80) of a file already read; at
  most one note per result and 8 per request (state reset with the repetition table at each new request).
  The index must already exist: in the default mode the note reads the graph section of the coder store of the working
  folder when there is one (it never creates an index or a graph; a folder that was never indexed gets no note); in
  the old mode it is used if the process has the old index in memory (any earlier `Find`) or if the working folder
  already has `.katana/cache` (then it is loaded once per process); the loop never creates cache files for a folder
  that never used `Find`. `KATANA_NEIGHBORS=0` turns the note off. `Find` with `mode=neighbors` and a path in `query`
  prints the note a `Read` of that file would get (research and test aid, not in the tool schema).
- *Limits and pending.* Portuguese questions against English code remain the biggest failure: the graph cannot bridge a
  seed that is wrong. Next experiment: a Portuguese-English term map taken from the folder itself (comments and
  identifiers) used to expand the query before BM25. The co-change edge is noisy in repositories with large commits and
  has beta 0.5. A file changed by the agent in this session is not in the graph until the next index update and rebuild.
- *Not verified.* The research numbers (R@5 15.9% to 33.0% in Python on 46 questions) were not reproduced in C; no
  comparison of the C ranking against the old ranking was run (no benchmark by the owner's decision); the neighbour note
  was exercised only through `mode=neighbors`, never inside a real agent loop with the model; whether the model follows the
  notes is unknown; Grep and Find result parsing was not run on real outputs of every output mode; Windows has no `Find`
  (stubs only); the 4.7k-file size and build time were not measured; the persisted graph was reloaded and a bad/old
  cache file was not injected by hand.

### Symbols (structural queries from the symbol index)
`Symbols(action, name, file, line, limit, depth, path)` (`src/tools/symbols.c`, work in `src/coder/symtool.c`): the
`katana coder def|refs|callers|callees|uses|outline|at|deps|impact` queries as one tool for the agent. Read-only (runs in
plan mode). One declaration with an `action` instead of nine tools, so the prompt does not grow: `Symbols` is in the
core list of `src/agent/prompt.c` (the lean list, 2026-11-23; it has a short form in `g_ct_short`), with one decision rule
against `Find` and `Grep` in the rules text. Nine alias tools were not added: each would cost a name in the by-name
list and a full declaration in every `ToolSearch` answer for no gain.
- `def` (`name`): where it is defined, `file:lines`, the signature line. `refs` (`name`): every occurrence by kind, each
  with the function it is in. `callers` / `callees` / `uses` (`name`): who calls it / what it calls in call order /
  occurrences that are not calls. `outline` (`file`): the symbols of a file, nested, with line ranges. `at` (`file`,
  `line`): the symbol containing the line and its enclosing symbols. `deps` (`file`): imports out and imported-by.
  `impact` (`name`, `depth` default 3, max 6): callers of callers (what a change can break). `flow` (`name` = a text key, or
  `file` [+ `line`], or nothing): links that go through TEXT, not through a call or import: who sends/emits/writes/calls a key
  and who handles/reads it (message `type` values, event names, HTTP routes, environment variables, config keys, subcommands).
  `flow name=register` lists PRODUCE and CONSUME sites (`file:line`, the enclosing function, the line) and the producer ->
  consumer LINKS with a confidence; `flow file=bridge/grey-channel.mjs` lists what that file sends and receives and where
  each key goes; `flow` with no target summarizes file -> file links by key. The answer ends with `Next: Read <file>
  offset=.. limit=..` for the consumer side, and says `Nobody handles it` / `Nobody sends it` (with the limit of the indexed
  set) when one side is missing. Default `limit` 5 per side (max 100); text mode is brief (the confidence explanation is
  cut to its first clauses). Details and heuristics: `docs/code-intelligence.md`, section "Links by text key".
- `tree` (`file` = the folder to show, default the project folder; `depth` default 2, max 4; `limit` default 40 folder lines, max 100;
  `path` = project folder as for the other actions): the folders of the project, breadth first (larger first inside a level),
  answered ONLY from the index of the tree (directories, counts, the `STS_DIRS` records and the live documents): no file is
  read and it never waits for a deeper level. It does not build an index: with none it says how to build one. One line per
  folder: `path/  N files (kotlin 166, js 48, other 12)  [symbols and links]  doc: <first line of its README or KATANA.md>`,
  and under it `main: a, b, c` (up to 4 definitions that other folders reference most, only when the folder is fully at
  symbols; names of 1-2 characters and `main` are left out). The bracket is how deep the index of that folder is: `[names
  only]`, `[text]` (`symbols not ready` follows), `[symbols]`, `[symbols and links]`. Ignored folders are always listed
  with the reason and the count (`node_modules/  (ignored: built-in heavy folder "node_modules", 12,340 files)`; `not
  counted yet` before the counting pass; `(has its own index; use path=...)` for a nested init), do not use `limit` (at most
  15 are listed, the rest is counted) and end with `ignored: N folders, M files not in the index. To search inside one, set
  `path` to it.` The text is cut at about 6500 characters, so the folder lines stop at about 4800 and the closing lines are
  kept (`(N more folders not shown; raise `limit` or call tree on a subfolder)`). Also `katana coder tree [folder] [--depth N]
  [--limit N] [--json]`. The `doc:` lines exist only for an index built by the crawler (`STS_DIRS`); an index built in
  process by `Find`/`Symbols`/`katana coder index` has no directory records, so those lines are absent there. Output size
  measured once on three trees (chars / about 4 chars per token): this repository 3.8 KB (960 tokens) at depth 2, limit 40;
  a 373-file project 2.2 KB (550); a 20-thousand-file synthetic one 2.2 KB (540); depth 4 and limit 100 is cut at 4.9 KB.
- Status lines (J.2 of `docs/code-index-lifecycle.md`), at most ONE, first line of the answer of `Find` and `Symbols`, only when the
  queried scope is not complete or not fresh; with a complete index on a small project the output is byte-identical to the
  previous one (checked on 8 queries in two projects). In order of importance: `[index was damaged and has been rebuilt]`;
  the one-time note of a restricted include (below); `[index: src/foo/ is still at level names only; text and symbols are not ready
  yet]` or `... is still being built: text ready for A of N files, symbols for B; results are partial]` or `[index: src/foo/ is
  still at level text; symbols not ready yet]` (the scope is the queried folder, or `the project`); and `[index: N files changed
  since indexed; shown from the files]` (now also in `Find`). The partial lines need the crawler to have a crawl record that is not
  complete (`STS_CRAWL`), so they appear in a session (watcher on), not after a synchronous build. `NOT FOUND` with a partial
  scope says `NOT FOUND so far` and `The index ... is still being built ...: this is not proof that it does not exist; try Grep, or
  ask again shortly.` (Find: `No match so far`, `NOT FOUND so far (literal)`, ...). `NOT FOUND` with ignored folders under the scope
  adds one line, `3 ignored folders could hold it: node_modules/, build/, ref/. To search one, set `path` to it.` (up to 3, the
  largest; `and others` when there are more; the ignored registry is counted by the pass that follows `katana coder index` and the
  crawler, and by a tool call that needs it and finds none; nothing inside the folders is read).
- Restricted include: `path` of `Find`, `Symbols` or `tree` that points INSIDE an ignored folder (`path=node_modules/express`) indexes
  just that folder (`scoped <folder>` in the `overrides` file next to the store, then one in-process update) and the answer starts with
  `[index: node_modules/express/ was ignored (built-in heavy folder "node_modules"); it is now indexed for queries whose path is inside
  it]`. Queries whose `path` is not inside it (the whole project) do not see its files. Refused, with a line saying so, when the folder has
  more than `KATANA_INDEX_INCLUDE_MAX_FILES` (default 50000) files; never for secret folders, nested indexes, `katana coder exclude`d
  folders or folders without an ignored-registry record. `Find` stops hiding the built-in heavy names (`node_modules`, `build`, `vendor`...)
  for paths below the folder asked in `path`.
- Output: `file:lines  kind name  [confidence N level; how]`, one snippet line per use, a one-line hint of the next
  query. There are no exact/derived/probable labels: the query is a cascade (exact name, then spelling variants, then
  edit distance <= 2; the answer says which step produced what it shows) and every result carries `confidence` 0-100,
  `level` (high >= 75, medium >= 45, low) and `how`. An empty result is `NOT FOUND: ...` with the limit of the indexed
  set (files indexed, files without symbol extraction, what the analysis does not see). Default `limit` per action:
  def 8, refs/callers/uses 15, callees 15, outline 100, at 8, deps 30, impact 25, flow 5 (max 100); the text is cut at about
  6500 characters. Measured on this repository: def 330 bytes, refs of a common helper 3.1 KB, callees 4.5 KB.
- Index: the coder index of the working folder (`<folder>/.katana`, shared with `Find`'s coder layer and
  `katana coder index`). It is created on the first call and updated incrementally on every call; the answer starts with
  `[symbol index created in N s]` when it was created, or updated and took more than 0.7 s (this repository: 1.3 s
  creation, 1413 files; a small project: 0.4 s). `path` points to another project folder. When the working folder is
  the home folder and `path` is empty the tool refuses, as `Find` does.
- `Find mode=refs` is now answered from the symbol index when the folder already has one and it knows the exact
  name as a definition (same output format as before: `Defined`, `Used N times in M files`, `line (in function): text`);
  otherwise, or with `KATANA_FIND_REFS=text`, it uses the old text index. Differences: the symbol index lists calls and
  other code references (not import lines or mentions in comments and documents), and names the exact enclosing
  function. `Find` (search) and `Symbols` both keep that index current: the coder update in `Find` now also builds the
  symbol sections, so a file changed after `katana coder index` is not left without symbols.
- Loop: `Symbols` counts as reading in `loop_plan_readonly` and `rr_covered` (identical repeated calls get the repeat
  note; it does not ask for a plan).
- Not verified: the model has never seen `tree` or the new lines (the v7 curriculum predates them; the v8 requirement is in section 5.1 of
  `docs/code-index-lifecycle.md`, phase 9); whether the model finds it through `ToolSearch` and uses it well (no run with the model); behaviour
  with several tool calls in parallel threads (the tool redirects stdout to capture the query output); Windows (not built:
  the tool answers "not available on Windows yet", as `Find` does).

### Rename, Replace, Patch (removed)
Removed from the table on 2026-11-23 (`docs/tools-review.md`, open question 2: 0 curriculum examples, 0 real calls; the
`x-rename` family teaches the manual way). The model changes many places with `Grep` plus `Edit` (`replace_all`) or a `Bash`
script. The code in `src/tools/multiedit.c` is owned by the old-index removal front.

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
`TodoWrite(todos, done)` writes the list and marks steps finished (`TaskCreate/Update/List/Get` were removed on
2026-11-23: a second way to write a plan that the model never chose). One list underneath (the one the desktop card shows). `AskUserQuestion(questions)`: 1 to 4 questions with 2 to 4 options
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
`ReportFindings`, `EndConversation`, `PowerShell`, and the MCP resource tools. `Bash`, `Write` and `Edit` (and, per file, `Rename`, `Replace` and `Patch`) obey the `never:`
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

## Changes of 2026-11-23 that affect what the model sees: the UIOE context block (phase U4)

Not measured, no run with the real model. Design and the normative format: `docs/uioe.md` (3.9 the Context Builder, 4.6 the block).

- **A user message can end with a context block.** Before the model runs, `uioe_prepare` (`src/uioe/api.c`) interprets the request, retrieves
  with the engines the tools use (`Find`, `Symbols`, `Read` are unchanged) and, behind a conservative door, appends
  `[katana: context for this request. ...]` ... `[end of context]` after the user's text. The labels are fixed and in English whatever the
  language of the request: `Question understood as`, `Answer path` (or `Findings`), `Evidence`, `Links`, `Not found / limits` (always present, each
  clause says why), `Next` (imperative, with a stop criterion). `[C]` is the confidence of a line that was re-read in the file; `(guess)` is an
  unchecked line. At most 1500 tokens (4 bytes per token), the most relevant first. Text that comes from files is escaped (`[katana` and
  `[end of context` get a space after the bracket, the chat-format markers are neutralized, secret shapes are masked).
- **The door.** Nothing is injected for a greeting, a vague or ambiguous request, a request that already names the exact file, a retrieval below
  confidence 80 (`UIOE_PACK_MIN_CONF`, `src/uioe/api.c`), one with no re-read support, a connector that timed out, or a step that took longer than
  `KATANA_UIOE_TOTAL_MS` (1500). Only code and file requests are served. A follow-up that only refers back ("and where does that go?") inherits the
  last investigation (a trace line says so) and gets a block only if the path holds something not already shown.
- **What it replaces.** With a block, the cited-folder note (`place_note`) and the project documents note (`ds_select`) are not appended: the
  folder is in `Question understood as` and `Next`, and the document lines the selector picks are `Evidence`. Without a block the old notes are
  appended exactly as before. The system prompt is not touched.
- **Switches.** `KATANA_UIOE=pack` (default) or `off` (the old behaviour, byte for byte). `KATANA_UIOE_LIMITS=1` also injects the short form
  (`Question understood as`, `Not found / limits`, `Next`) when retrieval is incomplete. `katana uioe ask --pack "<request>"` prints the block
  the agent would receive; `~/.katana/uioe.log` keeps the decision of the door and the block of every request; `KATANA_UIOE_DUMP=<file>` (tests)
  appends the user message exactly as it reaches the model.
