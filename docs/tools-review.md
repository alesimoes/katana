# Tool set review (2026-11-23)

The agent declares 12 tools in the prompt: `Bash, Read, Edit, Write, Find, Symbols, Grep, TodoWrite, AskUserQuestion, WebSearch, WebFetch, ToolSearch`
(`Symbols` has a short form in `g_ct_short`). Everything else is reachable by name through `ToolSearch`. Nothing in this document was measured:
the sizes were counted from the code and from `katana agentsys`; statements marked **(opinion)** are judgments.

Related: `docs/uioe-code-audit.md`, `docs/tools.md`, `docs/curriculum.md`, `docs/code-index-lifecycle.md` (the `Symbols tree` design).


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
| "More tools exist ..." paragraph (29 names) | 805 |
| the Find/Grep/Glob/Symbols/Read rule paragraph | 811 |
| the whole rules-and-tools part (from "You are katana" to "# Environment") | 4086 |

### 3.2 Short form (what is declared today; `g_ct_short` in `table.c`)

| tool | chars | | tool | chars |
|---|---|---|---|---|
| Edit | 613 | | WebFetch | 303 |
| Grep | 581 | | ToolSearch | 301 |
| Bash | 541 | | WebSearch | 302 |
| AskUserQuestion | 500 | | Write | 289 |
| Find | 490 |
| TodoWrite | 354 | | Read | 342 |

### 3.3 Long form (what `ToolSearch` returns, and what `ct_decl` uses for anything without a short form)

Largest: Grep 3436, Symbols 2743, Edit 1875, WebFetch 1718, WebSearch 1508, Read 1505, LSP 1424,
NotebookEdit 1408, Find 1406, CronCreate 1335, Bash 1333, ReportFindings 1278, AskUserQuestion 1288. Smallest: CronList 271, CronDelete 300, EnterPlanMode 363, SubagentHandback 404, TaskStop 433. The others are between 457 and 1095.

**All 46 in the long form: 45,935 characters, about 11k tokens (opinion).** The same 12 declared ones in the short form are 4893.
`docs/tools.md` already records the same effect ("19k with the full descriptions, 8.8k short form" of the older, whole prompt).
Per `ToolSearch` fetch the cost is the long form of the tool (194 to 3436 characters), paid once per session per tool, and a
`select:` of several names in one call costs the sum.

Two facts the sizes expose:

- `Symbols` is the second most expensive declaration (2743) and **has no short form**, which is part of why it is not in
  `core[]`. A short form of about 500 characters like Find's would fit it in the central list.
- The short forms drop all argument descriptions (`Find` short has `query, path, limit, mode` with no text); for the working
  model this is irrelevant (it has no declarations), for a base model it is the only description it gets.

## 6. The proposed central list (12)

### 6.1 The list

`Bash, Read, Edit, Write, Find, Symbols, Grep, TodoWrite, AskUserQuestion, WebSearch, WebFetch, ToolSearch`

It is the present list (12 declared, 13 with `Skill`) with `Symbols` in and `Glob` out. The conditional `Skill` declaration goes
away: the environment line already lists the skills (`sk_env_line`).

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
| Read | none to the arguments; **let `Read` of a folder return the listing** (names, sizes, dates) instead of an error | `Read` says "It cannot list directories"; a folder listing has no home besides `Bash ls` |
| Edit | none; keep `edits` | curriculum 6 uses; `replace_all` 38 |
| Write | none | |
| Grep | the short form already has 7 properties of 15; keep. The 3436-char long form is paid once per fetch, never in the working prompt | `output_mode` in 218 of 248 calls |
| TodoWrite | **make `done` safe**: if there is no list, the error should say how to write the first list in the model's own words, and the curriculum has a family for it: "done before todos" was the only error of the 12 real calls | section 1.3 |
| AskUserQuestion | none | 23 calls, 0 errors |
| WebSearch / WebFetch | none | 25 and 28 calls |
| ToolSearch | **make a non-`select:` query useful**: the model sent `katana` and `select:katana` (4 calls) for a project name. A query that matches no tool could answer "No tool named katana. Tools by name: ..." with the 29 names, instead of `No matching tools` | 4 real calls, 4 for a project name |

## 9. Risks and what NOT to change without retraining

**Do not change without retraining the model** (the names and keys are in the model's weights, not in the prompt, because of
`--no-tool-decl`):

- The **name** of any tool in the curriculum's 14 big tools.
- The **argument keys**: Read `file_path`, `offset`, `limit`; Edit `file_path`, `old_string`, `new_string`, `replace_all`, `edits`; Write
  `file_path`, `content`; Bash `command`, `description`, `run_in_background`; Grep `pattern`, `path`, `glob`, `output_mode`, `-i`, `-n`,
  `-A`; Find `query`, `path`, `mode`; Symbols `action`, `name`, `file`, `line`, `depth`, `path`; TodoWrite
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
3. **Machine-wide location has no tool**: `Find` refuses the home folder; a folder the model cannot locate has to be found with `Bash`.
4. **The Windows build** has no `Find`, `Symbols`, `LSP`, terminal or jobs (`#ifndef _WIN32` in the audit's weak point 4): a central list
   with two code tools has no code tool on Windows. The list must stay per platform.
5. **TodoWrite's `done` shortcut** is 436 of its 677 curriculum uses, and the single failure mode of the real calls; it should be
   safe on the engine side before anything else (section 6.3).
