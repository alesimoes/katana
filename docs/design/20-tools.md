# Tools

What the model can call, what comes back, and how many rounds a piece of work takes. Numbers from
`01-token-budget.md`: results are 63% of all tokens, `Bash` output alone 30%, and each round carries 67 tokens of
fixed overhead.

## Where katana is today

- 44 tools in `src/tools/` (table and dispatcher in `src/tools/table.c`), 11 declared in full in the prompt, the
  rest by name with `ToolSearch`. 1,244 model-free checks. No measurement with a real model (`docs/tools.md`).
- Results are wrapped in two Portuguese lines (`agent()` in `src/agent/loop.c`, the `snprintf` after `defang`):
  134 bytes, about 39 tokens per result, 8,250 tokens in the reference session.
- `Bash` returns up to 30,000 characters inline on success and 10,000 on failure (`src/tools/bash.c`), further
  capped by the context left (`g_budget`). `Read` returns numbered lines, 2,000 by default.
- `Edit` is exact string replacement and requires a prior full read; the result echoes the changed lines; a syntax
  check runs after writes for six languages (`src/agent/code.c`).
- Large reads are replaced by a model-written note of up to 250 words (`tool_digest`, `src/agent/delegate.c`, on by
  default, never measured; one earlier measurement: 63 s to digest 5.8 KB against about 45 s to read it raw).
- Web search returns passages in one round (2026-11-21).

## What the frontier does

- **Deployed.** Guidance from a hosted vendor: few consolidated tools, a concise result by default with detail on
  request (their example: one third of the tokens), truncation that steers the next call, errors that say what to
  change [1]. Another vendor measured its own savings on real traffic: removing line-number prefixes from file views
  3 to 5%, compressing only build, test and lint noise 5.5%, and found that aggressive shortening **raised** total
  cost because the agent re-read and re-ran [10]. Long outputs written to files that the agent greps or tails, with
  tool descriptions also kept as files: 47% fewer tokens in one A/B, only on runs that used external tools [11].
  Calling tools from code the model writes: 37% fewer tokens on average and many inference passes removed in vendor
  examples [2][3][4]. Search: the largest coding agents use plain text search driven by the model; one vendor
  reports a 12.5% accuracy gain from adding a semantic index on its own benchmark [9].
- **Published.** Line-anchored edits (each line shown with a short hash that the edit refers to) beat diff patches
  for 14 of 16 models and string replacement for 12 of 16 on a synthetic benchmark, with the largest gains on weak
  models and 17 to 61% fewer output tokens [5]; an independent replication found no clear winner and a language
  dependence [6]. Code inside JSON strings lowers code quality [7]. Small-model harnesses report that compound tools
  collapse 8 to 15 calls into 1 or 2 [67]. With a strong shell user, shell-only matches a full tool set at lower
  cost; predefined tools help models that are weak at the shell [78].
- **Unsolved.** No controlled comparison of edit formats on real tasks for small open models. No measurement of
  code-as-interface on a model of this size. Lossy result compression has no method that is safe by construction.

**For a slow engine:** shape results without a model, never discard what cannot be fetched back, and remove rounds.

## Design

### Now

**T1. A three-token result wrapper.**
- What: `<tool_response>\n<result>\n</tool_response>` for ChatML and the native response form for Gemma 4, with
  no extra lines; the tool name is already in the call above. The injection defence stays in `defang` (which
  neutralises markers) and in one system-prompt sentence.
- Mechanism: P2; about 36 tokens per result removed. Cost: two `snprintf` formats in `loop.c`, the matching readers in
  `context.c` (`ah_shrink_tool` looks for the Portuguese marker) and in `src/app/importer.c`; sessions recorded with
  the old wrapper are re-rendered by `katana import`. One day.
- Measure: tokens read per result; failed-result rate must not rise (the wrapper may be helping the model find the
  result's end). Drop if failures rise by more than 2 points on the micro-set.

**T2. Model-free result shaping by command class.**
- What: a shaper between the tool and the context, selected by the tool and, for `Bash`, by the program of the
  command (reusing `bk_prog` in `loop.c`):

  | class | shape |
  |---|---|
  | listing (`ls`, `find`, `tree`, `Glob`) | names only, grouped by folder, counts for folders over 20 entries |
  | build, test, lint | the verdict line plus the first failing unit (file:line and message), counts of the rest |
  | install, download, archive | last line and exit code |
  | help text (`--help`, `-h`, `man`) | usage line and the options that match words of the current plan step; the rest behind the handle |
  | version control | status and log as is; diffs as is up to 200 lines |
  | file content (`cat`, `Read`), diffs | never shaped (code is content) |
  | unknown | head 40 lines, tail 20 lines, count of the middle |

  Everything removed stays reachable (T3). `tool_digest` is switched off by default: it spends generation (40 ms
  per token) to save reading (15 ms per token), which only pays if the note is under a third of the text and is
  re-read later.
- Mechanism: 30% of the session is command output, most of it noise the model must still read at 15 ms a token.
- Evidence: [10] (which also sets the rule: compress noise, never content), [1].
- Cost: `src/tools/shape.c`, about 400 lines, a table of program names to classes; hook in `agent_tool`
  (`src/agent/dispatch.c`). One week with tests on recorded outputs from the session files.
- Measure: tokens read per solved task and **total** seconds per solved task, shaping on and off, on the micro-set
  plus the terminal tasks. Drop a class whose shaping raises rounds per task (the model came back for what was
  cut).

**T3. Results behind handles.**
- What: every result larger than 60 lines is stored by the engine as `r<N>` (a file under the session folder). The
  model sees the shaped form and the line `[r17: 412 lines]`. `Read` and `Grep` accept a handle where they accept a
  path (`Read(file_path: "r17", offset: 200, limit: 40)`, `Grep(pattern, path: "r17")`).
- Mechanism: truncation becomes safe because nothing is lost, and the way back is two tools the model already
  knows instead of a new tool (no declaration tokens).
- Evidence: [11] for outputs kept as files; here the handle is a name inside existing tools.
- Cost: path resolution in `ct_f_abs` (`src/tools/read.c`) and in `grep.c`; storage in `dispatch.c`. Three days.
- Measure: how often a handle is opened (under 10% means the shapes are right; over 40% means they cut too much).
  Drop: if handles are never opened, keep the storage and remove the line.

**T4. "Unchanged" instead of a second read.**
- What: `Read` of a file whose bytes are still in the context (the engine knows: same size and date, and the
  result has not been masked or compacted away) returns `unchanged since round 14, still above` (8 tokens).
  After the content left the context, the read returns it normally.
- Mechanism: 7,550 tokens of the reference session. The record exists (`ct_seen_mark` in `src/tools/common.c`,
  `rd_*` in `src/agent/code.c`); what is missing is the link to what is still in context.
- Cost: one field per seen file (the round and whether it was evicted), set by `context.c` when a result is
  shrunk. Two days. Measure: re-read tokens per session. Drop if the model answers the stub by reading through
  `Bash` (`cat`), which would cost more.

**T5. `Bash` description becomes optional and engine-written.** The desktop's activity line is produced by a table
(verb of the command to a phrase) when the model gives none; the declaration and the curriculum stop asking for it.
About 10 tokens per `Bash` call, 137 calls. Measure: tokens per `Bash` call. Drop if the owner finds the generated
lines worse than the model's.

**T6. A tool set that fits the model.** Declare 8 tools in full (`Bash`, `Read`, `Edit`, `Write`, `Grep`, `Glob`,
`TodoWrite`, `WebSearch`) and move `AskUserQuestion`, `WebFetch` and `ToolSearch` to the by-name list. Measured
first: plan item B4 (first set against second set) extended with this 8-tool arm. Evidence: [1][67][78]. Drop if the
8-tool arm solves fewer tasks.

### Next

**V1. Verdict-first edits (original in this form).**
- What: after a successful `Write` or `Edit`, the engine itself runs, in order and with a time limit: the syntax
  check (exists), the project's `lint:` command restricted to the file when the brief declares one, and the
  project's `test:` command when the brief declares one and it ran under 20 s last time. The edit's result is one
  line: `ok: syntax, 14 tests pass (3.1 s)` or `fail: test_parse_empty (tests/test_p.py:41): expected [] got
  None` with the log behind a handle.
- Mechanism: P1 and P3. Today the model edits, then spends a round calling the tests, then reads the log. The
  verdict removes that round and replaces the log by its only informative line. The model reads a verdict before it
  can build on a broken edit.
- Evidence: syntax checks after edits are deployed in several agents; running the project's tests inside the edit
  result and returning only a verdict was not found in any shipped system (landscape report, topic 2).
- Cost: `src/tools/verify.c` (about 250 lines): command from `brief.c`, runner from `bash.c`, per-language parsers
  for the first failure (pytest, unittest, cargo, go test, make/cc, a generic `file:line: message` regex). Hook at
  the end of `ct_edit` and `ct_write`. One week.
- Measure: rounds and seconds per solved task on `extend-*` and the 15 fix tasks, on and off. Count how often the
  model still runs the tests itself right after (wasted if so). Drop if rounds per task do not fall by 10%.
- Confidence: high.

**B1. Batch programs: several dependent calls in one round.**
- What: a tool `Do` whose single argument `steps` is text, one call per line, in a closed mini-language:

  ```
  1 Grep pattern="parse_header" path="src" output_mode="files_with_matches"
  2 Read file_path=$1.first limit=80
  3 Bash command="make test" ?
  return 2 3
  ```

  `$N` is the result of step N (`.first`, `.lines`, `.path` selectors for list results), a trailing `?` lets the
  batch continue when that step fails (default: stop at the first failure), `each $1 as f: Read file_path=$f
  limit=40` runs a step over at most 8 items, `return` names the results the model wants to read. No variables, no
  arithmetic, no loops beyond `each`. The engine runs the steps through the normal dispatcher, with the same
  permissions and rules, and returns only the named results (shaped) plus one status line per other step.
- Mechanism: P3. Dependent calls are the case several-calls-per-turn cannot cover (the second call needs the first
  one's result). Each avoided round saves 67 tokens of overhead, a decision, and the intermediate result the model
  did not need to read.
- Evidence: code-as-tool-interface [2][3][4] gets its savings this way, with a general language and a sandbox.
  Small models write short declarative lines more reliably than programs (unmeasured assumption), and a closed
  language can be enforced by the call grammar (`30-call-formats.md`), so a batch is valid by construction.
- Cost: `src/tools/do.c` (parser and runner, about 350 lines), a grammar branch in `callgrammar.c`, a curriculum
  family. Two weeks.
- Measure: rounds per solved task with `Do` declared and not; how often the model uses it; failure rate of
  batches. Drop if use is under 5% of rounds after one night of curriculum, or if batch failures exceed single-call
  failures.
- Confidence: medium. It depends on whether a 3-billion-active-parameter model plans three calls ahead.

**T7. Anchored edits for long replacements.**
- What: `Read` can show lines as `41:a3| text` (line number and a two-character hash of the line); `Edit` accepts
  `at: "41:a3-47:0c"` instead of `old_string`. The engine rejects a stale anchor and returns the current lines.
- Mechanism: an `Edit` costs 337 tokens per call today, about half of it reproducing `old_string`; an anchor is 6
  tokens and cannot mismatch on whitespace (3 of the 10 edits in the reference session failed on a mismatch).
- Evidence: [5] for, [6] against. Cost: `read.c` and `edit.c`, 150 lines, plus grammar and curriculum. Measure: an
  edit micro-benchmark (30 mechanical edits on files of the codebench projects): success rate and tokens written
  per edit, anchors against strings, both models. Drop if success is not higher by 10 points or tokens lower by
  30%; the published evidence is contested, so this is an experiment, not a default.

### Beyond

**T8. Structural reads.** `Read(file_path, symbol: "parse_header")` returns one function with its signature
context, found by a per-language outline (regular expressions per language, no parser dependency; the language
server tool exists but needs a server installed). Removes "read 2,000 lines to see 30". Measure: tokens read per
`Read`. Confidence: medium.

**T9. Results as engine-side values the model computes on.** An extension of handles in which `Do` steps can
filter and count (`count $1`, `filter $1 "error"`), so questions such as "how many tests fail" never bring the
list into context. This is the closed-language version of what hosted agents do with a code sandbox; it stays
within the decision that everything is C inside the binary. Confidence: medium, after B1 proves itself.

**Rejected:** a semantic (embedding) index (needs a second model in memory and upkeep; the evidence is one vendor's
benchmark [9]); a model that applies edits (a second generation per edit); table-like result encodings for
model-written text (independent studies found accuracy losses in multi-turn use [54]); a general scripting
language as the tool interface (needs a sandbox and a model that writes correct programs; B1 is the bounded form).
